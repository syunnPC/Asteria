// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Firmware::PalBootstrapCapabilities;
using Platform::Ia64::Firmware::PalBootstrapCapabilityCaptureError;
using Platform::Ia64::Firmware::PalBootstrapCapabilityCapturePhase;
using Platform::Ia64::Firmware::PalStaticCallAdapterError;
using Platform::Ia64::Interrupt::GetNativeIa64ApplicationProcessorTakeoverCoordinator;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverClaimError;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinator;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverCoordinatorState;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverFailure;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverFailurePhase;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverInitializationError;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverLifecycle;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverProcessorSnapshot;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverProgressSnapshot;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverPublicationError;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverRecord;
using Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverSnapshotError;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerState;
using Platform::Ia64::Interrupt::Ia64LocalSapicLogicalProcessor;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageDescriptor;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwner;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwnerState;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageSizeBytes;
using Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus;
using Platform::Ia64::Memory::InitialTranslationInstallError;
using Platform::Ia64::Memory::PalTranslationCachePurgeDecodeError;
using Platform::Ia64::Memory::PalVirtualMemoryDecodeError;

constexpr Foundation::UInt32 FirmwareDescriptorSize{40U};
constexpr Foundation::UInt32 ConventionalMemoryType{7U};
constexpr Foundation::UInt64 WriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PibPhysicalBase{0x0000'0000'FEE0'0000ULL};
constexpr Foundation::UInt64 PibAccessBase{PibPhysicalBase | PhysicalAddressUncachedBit};
constexpr Foundation::Size TestStoragePageCount{64U};

alignas(32U * 1024U) Foundation::UInt8 g_storage[TestStoragePageCount * Kernel::Memory::PhysicalPageSizeBytes];
Boot::BootInfo g_bootInfo{};

[[nodiscard]] Foundation::UInt64 StoragePhysicalStart() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_storage));
}

[[nodiscard]] constexpr Foundation::UInt64 InterruptAddress(const Foundation::UInt8 hardwareId,
                                                            const Foundation::UInt8 hardwareEid) noexcept
{
    return PibAccessBase + (static_cast<Foundation::UInt64>(hardwareId) << 12U) +
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

[[nodiscard]] Ia64LocalSapicIpiControllerSnapshot
MakeControllerSnapshot(const Foundation::UInt8 processorCount) noexcept
{
    Ia64LocalSapicIpiControllerSnapshot snapshot{};
    snapshot.PhysicalBaseAddress = PibPhysicalBase;
    snapshot.AccessBaseAddress = PibAccessBase;
    snapshot.ProcessorCount = processorCount;
    snapshot.EnabledProcessorMask = static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
    snapshot.BaseKind = Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical;
    snapshot.State = Ia64LocalSapicIpiControllerState::Ready;
    for (Foundation::UInt8 processor{0U}; processor < processorCount; ++processor)
    {
        snapshot.Processors[processor] =
            MakeProcessor(100U + processor, static_cast<Foundation::UInt8>(0x20U + processor),
                          static_cast<Foundation::UInt8>(0x30U + processor));
    }
    return snapshot;
}

[[nodiscard]] bool InitializeAllocator(BootstrapPageAllocator& allocator) noexcept
{
    g_bootInfo = Boot::BootInfo{};
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(Boot::BootInfo));
    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = FirmwareDescriptorSize;
    g_bootInfo.FirmwareMemoryMapSizeBytes = FirmwareDescriptorSize;
    g_bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = StoragePhysicalStart() + Kernel::Memory::PhysicalPageSizeBytes,
        .VirtualStart = 0U,
        .PageCount = TestStoragePageCount - 1U,
        .Attributes = WriteBackAttribute,
        .FirmwareType = ConventionalMemoryType,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
    return allocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None;
}

[[nodiscard]] bool PrepareOwner(ApplicationProcessorExecutionStorageOwner& owner, BootstrapPageAllocator& allocator,
                                const Foundation::UInt8 processorCount) noexcept
{
    if (!InitializeAllocator(allocator))
    {
        return false;
    }
    const Ia64LocalSapicIpiControllerSnapshot snapshot = MakeControllerSnapshot(processorCount);
    return owner.Prepare(allocator, snapshot).HasValue();
}

template <typename T> [[nodiscard]] bool BytesEqual(const T& first, const T& second) noexcept
{
    const auto* const firstBytes = reinterpret_cast<const Foundation::UInt8*>(&first);
    const auto* const secondBytes = reinterpret_cast<const Foundation::UInt8*>(&second);
    for (Foundation::Size index{0U}; index < sizeof(T); ++index)
    {
        if (firstBytes[index] != secondBytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool RecordIsZero(const Ia64ApplicationProcessorTakeoverRecord& record) noexcept
{
    return (record.InitialStackPointer == 0U) && (record.RseBackingStorePhysicalStart == 0U) &&
           (record.ProcessorValue == 0U);
}

[[nodiscard]] bool FailureIsZero(const Ia64ApplicationProcessorTakeoverFailure& failure) noexcept
{
    const Ia64ApplicationProcessorTakeoverFailure zero{};
    return BytesEqual(failure, zero);
}

[[nodiscard]] bool CapabilitiesAreZero(const PalBootstrapCapabilities& capabilities) noexcept
{
    const PalBootstrapCapabilities zero{};
    return BytesEqual(capabilities, zero);
}

[[nodiscard]] bool SnapshotPayloadIsZero(const Ia64ApplicationProcessorTakeoverProcessorSnapshot& snapshot) noexcept
{
    return CapabilitiesAreZero(snapshot.PalCapabilities) && FailureIsZero(snapshot.Failure) &&
           (snapshot.Reserved[0U] == 0U) && (snapshot.Reserved[1U] == 0U) && (snapshot.Reserved[2U] == 0U) &&
           (snapshot.Reserved[3U] == 0U) && (snapshot.Reserved[4U] == 0U) && (snapshot.Reserved[5U] == 0U) &&
           (snapshot.Reserved[6U] == 0U);
}

[[nodiscard]] constexpr bool
RecordMatchesDescriptor(const Ia64ApplicationProcessorTakeoverRecord& record,
                        const ApplicationProcessorExecutionStorageDescriptor& descriptor) noexcept
{
    return (record.InitialStackPointer == descriptor.InitialStackPointer) &&
           (record.RseBackingStorePhysicalStart == descriptor.RseBackingStorePhysicalStart) &&
           (record.ProcessorValue == descriptor.Processor.GetValue());
}

[[nodiscard]] constexpr PalBootstrapCapabilities MakeCapabilities(const Foundation::UInt64 tag) noexcept
{
    PalBootstrapCapabilities capabilities{};
    capabilities.VirtualMemory.Translation.InsertablePageSizeMask = 0x0000'0001'5555'7000ULL ^ tag;
    capabilities.VirtualMemory.Translation.PhysicalAddressBits = 50U;
    capabilities.VirtualMemory.Translation.ImplementedVirtualAddressMostSignificantBit = 60U;
    capabilities.VirtualMemory.Translation.RegionIdentifierBits = 24U;
    capabilities.VirtualMemory.Translation.ProtectionKeyBits = 8U;
    capabilities.VirtualMemory.Translation.MemoryAttributeMask = 0x71U;
    capabilities.VirtualMemory.Translation.HardwareWalkerImplemented = true;
    capabilities.VirtualMemory.PurgePageSizeMask = 0x0000'0001'5555'7000ULL;
    capabilities.VirtualMemory.InstructionTranslationRegisterCount = 8U;
    capabilities.VirtualMemory.DataTranslationRegisterCount = 8U;
    capabilities.VirtualMemory.ProtectionKeyRegisterCount = 16U;
    capabilities.VirtualMemory.MaximumConcurrentPurgesEncoding = 2U;
    capabilities.VirtualMemory.HashTagIdentifier = 1U;
    capabilities.VirtualMemory.UniqueTranslationCacheCount = 4U;
    capabilities.VirtualMemory.TranslationCacheLevelCount = 2U;
    capabilities.TranslationCachePurge.BaseAddress = 0x8000U + tag;
    capabilities.TranslationCachePurge.OperationCount = 6U;
    capabilities.TranslationCachePurge.LastPurgeAddress = 0xD000U + tag;
    capabilities.TranslationCachePurge.OuterCount = 2U;
    capabilities.TranslationCachePurge.InnerCount = 3U;
    capabilities.TranslationCachePurge.OuterStride = 0x1000U;
    capabilities.TranslationCachePurge.InnerStride = 0x2000U;
    return capabilities;
}

[[nodiscard]] constexpr PalBootstrapCapabilityCaptureError MakePalFailure() noexcept
{
    return PalBootstrapCapabilityCaptureError{
        .Phase = PalBootstrapCapabilityCapturePhase::TranslationCachePurgeDecoding,
        .AdapterError = PalStaticCallAdapterError::None,
        .VirtualMemoryError = PalVirtualMemoryDecodeError::None,
        .TranslationCachePurgeError = PalTranslationCachePurgeDecodeError::OperationLimitExceeded,
    };
}

[[nodiscard]] bool IsInitialProgress(const Ia64ApplicationProcessorTakeoverProgressSnapshot& snapshot,
                                     const Ia64ApplicationProcessorTakeoverCoordinatorState state) noexcept
{
    bool slotsUnused{true};
    for (const Ia64ApplicationProcessorTakeoverLifecycle lifecycle : snapshot.Processors)
    {
        slotsUnused = slotsUnused && (lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Unused);
    }
    return slotsUnused && (snapshot.ProcessorCount == 0U) && (snapshot.EnabledProcessorMask == 0U) &&
           (snapshot.TerminalMask == 0U) && (snapshot.FailedMask == 0U) && (snapshot.State == state);
}

[[nodiscard]] bool IsPublishedProcessorSnapshotValid(const Ia64ApplicationProcessorTakeoverProcessorSnapshot& snapshot,
                                                     const Foundation::UInt8 processor,
                                                     const PalBootstrapCapabilities& capabilities,
                                                     const InitialInterruptVectorTableApInstallStatus ivtFailure,
                                                     const InitialTranslationInstallError translationFailure) noexcept
{
    if (snapshot.Takeover.ProcessorValue != processor)
    {
        return false;
    }

    if (processor == 1U)
    {
        if (snapshot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished)
        {
            return BytesEqual(snapshot.PalCapabilities, capabilities) && FailureIsZero(snapshot.Failure);
        }
        return (snapshot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled) &&
               BytesEqual(snapshot.PalCapabilities, capabilities) && FailureIsZero(snapshot.Failure);
    }
    if (processor == 2U)
    {
        if (snapshot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Claimed)
        {
            return SnapshotPayloadIsZero(snapshot);
        }
        return (snapshot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed) &&
               CapabilitiesAreZero(snapshot.PalCapabilities) &&
               (snapshot.Failure.Phase ==
                Ia64ApplicationProcessorTakeoverFailurePhase::InterruptVectorTableInstallation) &&
               (snapshot.Failure.InterruptVectorTableInstallationStatus == ivtFailure) &&
               BytesEqual(snapshot.Failure.PalCapabilityCapture, PalBootstrapCapabilityCaptureError{}) &&
               (snapshot.Failure.InitialTranslationInstallationError == InitialTranslationInstallError::None);
    }
    if (processor == 3U)
    {
        if (snapshot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished)
        {
            return BytesEqual(snapshot.PalCapabilities, capabilities) && FailureIsZero(snapshot.Failure);
        }
        return (snapshot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed) &&
               BytesEqual(snapshot.PalCapabilities, capabilities) &&
               (snapshot.Failure.Phase ==
                Ia64ApplicationProcessorTakeoverFailurePhase::InitialTranslationInstallation) &&
               (snapshot.Failure.InterruptVectorTableInstallationStatus ==
                InitialInterruptVectorTableApInstallStatus::Installed) &&
               BytesEqual(snapshot.Failure.PalCapabilityCapture, PalBootstrapCapabilityCaptureError{}) &&
               (snapshot.Failure.InitialTranslationInstallationError == translationFailure);
    }
    return false;
}

[[nodiscard]] bool AreConcurrentPublicationSnapshotsValid(
    const Ia64ApplicationProcessorTakeoverCoordinator& coordinator, const PalBootstrapCapabilities& capabilities,
    const InitialInterruptVectorTableApInstallStatus ivtFailure,
    const InitialTranslationInstallError translationFailure, bool& allTerminal) noexcept
{
    bool valid{true};
    for (Foundation::UInt8 processor{1U}; processor < 4U; ++processor)
    {
        const auto snapshot = coordinator.GetProcessorSnapshot(CpuId{processor});
        valid = valid && snapshot.HasValue() &&
                IsPublishedProcessorSnapshotValid(snapshot.GetValue(), processor, capabilities, ivtFailure,
                                                  translationFailure);
    }

    const Ia64ApplicationProcessorTakeoverProgressSnapshot progress = coordinator.GetProgressSnapshot();
    valid = valid && (progress.State == Ia64ApplicationProcessorTakeoverCoordinatorState::Ready) &&
            ((progress.TerminalMask & Foundation::UInt8{0xF1U}) == 0U) &&
            ((progress.FailedMask & Foundation::UInt8{0xF3U}) == 0U) &&
            ((progress.FailedMask & progress.TerminalMask) == progress.FailedMask) &&
            ((progress.Processors[1U] == Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished) ||
             (progress.Processors[1U] == Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled)) &&
            ((progress.Processors[2U] == Ia64ApplicationProcessorTakeoverLifecycle::Claimed) ||
             (progress.Processors[2U] == Ia64ApplicationProcessorTakeoverLifecycle::Failed)) &&
            ((progress.Processors[3U] == Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished) ||
             (progress.Processors[3U] == Ia64ApplicationProcessorTakeoverLifecycle::Failed));
    allTerminal = progress.TerminalMask == Foundation::UInt8{0x0EU};
    return valid;
}

void TestNativeSingletonAndNotReadyOwner(TestContext& runner) noexcept
{
    Ia64ApplicationProcessorTakeoverCoordinator& nativeFirst = GetNativeIa64ApplicationProcessorTakeoverCoordinator();
    Ia64ApplicationProcessorTakeoverCoordinator& nativeSecond = GetNativeIa64ApplicationProcessorTakeoverCoordinator();
    ASTERIA_EXPECT(runner, "the native accessor returns one constant-initialized coordinator",
                   &nativeFirst == &nativeSecond,
                   IsInitialProgress(nativeFirst.GetProgressSnapshot(),
                                     Ia64ApplicationProcessorTakeoverCoordinatorState::Uninitialized));

    ApplicationProcessorExecutionStorageOwner freshOwner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    ASTERIA_EXPECT(
        runner, "a Fresh owner fails one-shot initialization without slot publication",
        freshOwner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Fresh,
        coordinator.Initialize(freshOwner) == Ia64ApplicationProcessorTakeoverInitializationError::OwnerNotReady,
        IsInitialProgress(coordinator.GetProgressSnapshot(),
                          Ia64ApplicationProcessorTakeoverCoordinatorState::InitializationFailed),
        coordinator.Initialize(freshOwner) == Ia64ApplicationProcessorTakeoverInitializationError::AlreadyInitialized);
}

void TestFailedOwners(TestContext& runner) noexcept
{
    BootstrapPageAllocator malformedAllocator;
    ApplicationProcessorExecutionStorageOwner malformedOwner;
    Ia64ApplicationProcessorTakeoverCoordinator malformedCoordinator;
    const Ia64LocalSapicIpiControllerSnapshot malformedSnapshot{};
    const auto malformedPreparation = malformedOwner.Prepare(malformedAllocator, malformedSnapshot);
    ASTERIA_EXPECT(runner, "an owner failed by malformed controller input publishes no AP slot",
                   !malformedPreparation.HasValue(),
                   malformedOwner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Failed,
                   malformedCoordinator.Initialize(malformedOwner) ==
                       Ia64ApplicationProcessorTakeoverInitializationError::OwnerFailed,
                   IsInitialProgress(malformedCoordinator.GetProgressSnapshot(),
                                     Ia64ApplicationProcessorTakeoverCoordinatorState::InitializationFailed));

    BootstrapPageAllocator unavailableAllocator;
    ApplicationProcessorExecutionStorageOwner failedOwner;
    Ia64ApplicationProcessorTakeoverCoordinator failedCoordinator;
    const Ia64LocalSapicIpiControllerSnapshot validSnapshot = MakeControllerSnapshot(2U);
    const auto failedPreparation = failedOwner.Prepare(unavailableAllocator, validSnapshot);
    ASTERIA_EXPECT(
        runner, "a terminal allocation-failed owner is rejected without publication", !failedPreparation.HasValue(),
        failedOwner.GetState() == ApplicationProcessorExecutionStorageOwnerState::Failed,
        failedCoordinator.Initialize(failedOwner) == Ia64ApplicationProcessorTakeoverInitializationError::OwnerFailed,
        IsInitialProgress(failedCoordinator.GetProgressSnapshot(),
                          Ia64ApplicationProcessorTakeoverCoordinatorState::InitializationFailed));
}

void TestProcessorShape(TestContext& runner, const Foundation::UInt8 processorCount) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const bool prepared = PrepareOwner(owner, allocator, processorCount);
    const Ia64ApplicationProcessorTakeoverInitializationError initialized = coordinator.Initialize(owner);
    const Ia64ApplicationProcessorTakeoverProgressSnapshot progress = coordinator.GetProgressSnapshot();
    const Foundation::UInt8 expectedMask =
        static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);

    bool slotsExact{prepared && (initialized == Ia64ApplicationProcessorTakeoverInitializationError::None) &&
                    (progress.State == Ia64ApplicationProcessorTakeoverCoordinatorState::Ready) &&
                    (progress.ProcessorCount == processorCount) && (progress.EnabledProcessorMask == expectedMask) &&
                    (progress.TerminalMask == 0U) && (progress.FailedMask == 0U)};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const auto processorSnapshot = coordinator.GetProcessorSnapshot(CpuId{processor});
        slotsExact = slotsExact && processorSnapshot.HasValue();
        if (!processorSnapshot.HasValue())
        {
            continue;
        }

        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& slot = processorSnapshot.GetValue();
        if ((processor != 0U) && (processor < processorCount))
        {
            const auto descriptor = owner.GetDescriptor(CpuId{processor});
            slotsExact = slotsExact && descriptor.HasValue() &&
                         (progress.Processors[processor] == Ia64ApplicationProcessorTakeoverLifecycle::Prepared) &&
                         (slot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Prepared) &&
                         RecordMatchesDescriptor(slot.Takeover, descriptor.GetValue()) && SnapshotPayloadIsZero(slot);
        }
        else
        {
            slotsExact = slotsExact &&
                         (progress.Processors[processor] == Ia64ApplicationProcessorTakeoverLifecycle::Unused) &&
                         (slot.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Unused) &&
                         RecordIsZero(slot.Takeover) && SnapshotPayloadIsZero(slot);
        }
    }

    const auto invalidSlot = coordinator.GetProcessorSnapshot(CpuId{});
    ASTERIA_EXPECT(
        runner,
        (processorCount == 1U)
            ? "one CPU publishes no AP record and leaves every slot unused"
            : ((processorCount == 2U)
                   ? "two CPUs publish exactly one ordered AP takeover record"
                   : ((processorCount == 3U) ? "three CPUs publish exactly two ordered AP takeover records"
                                             : "four CPUs publish exactly three ordered AP takeover records")),
        slotsExact, !invalidSlot.HasValue(),
        invalidSlot.GetError() == Ia64ApplicationProcessorTakeoverSnapshotError::InvalidProcessorSlot,
        coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::AlreadyInitialized);
}

void TestClaimsAndLegalTransitions(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const bool setup = PrepareOwner(owner, allocator, 4U) &&
                       (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None);

    const auto wrong = coordinator.Claim(CpuId{});
    const auto bsp = coordinator.Claim(CpuId{0U});
    const auto first = coordinator.Claim(CpuId{1U});
    const auto duplicate = coordinator.Claim(CpuId{1U});
    const auto second = coordinator.Claim(CpuId{2U});
    const auto third = coordinator.Claim(CpuId{3U});
    const auto firstDescriptor = owner.GetDescriptor(CpuId{1U});
    const auto secondDescriptor = owner.GetDescriptor(CpuId{2U});
    const auto thirdDescriptor = owner.GetDescriptor(CpuId{3U});
    bool recordsExact{first.HasValue() && second.HasValue() && third.HasValue() && firstDescriptor.HasValue() &&
                      secondDescriptor.HasValue() && thirdDescriptor.HasValue()};
    if (recordsExact)
    {
        recordsExact = RecordMatchesDescriptor(*first.GetValue(), firstDescriptor.GetValue()) &&
                       RecordMatchesDescriptor(*second.GetValue(), secondDescriptor.GetValue()) &&
                       RecordMatchesDescriptor(*third.GetValue(), thirdDescriptor.GetValue()) &&
                       ((reinterpret_cast<Foundation::UIntPointer>(first.GetValue()) %
                         alignof(Ia64ApplicationProcessorTakeoverRecord)) == 0U);
    }
    ASTERIA_EXPECT(runner, "wrong, BSP, and duplicate claims are inert while exact records remain stable", setup,
                   !wrong.HasValue(), wrong.GetError() == Ia64ApplicationProcessorTakeoverClaimError::UnknownProcessor,
                   !bsp.HasValue(), bsp.GetError() == Ia64ApplicationProcessorTakeoverClaimError::BootstrapProcessor,
                   recordsExact, !duplicate.HasValue(),
                   duplicate.GetError() == Ia64ApplicationProcessorTakeoverClaimError::DuplicateClaim);

    const PalBootstrapCapabilities capabilities = MakeCapabilities(0x111U);
    constexpr InitialInterruptVectorTableApInstallStatus ivtFailure{
        InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch};
    const PalBootstrapCapabilityCaptureError palFailure = MakePalFailure();
    const Ia64ApplicationProcessorTakeoverPublicationError firstOwned = coordinator.PublishOwnedPhysical(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError firstPalPublished =
        coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities);
    const auto firstPalSnapshot = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverProgressSnapshot intermediateProgress = coordinator.GetProgressSnapshot();
    const Ia64ApplicationProcessorTakeoverPublicationError firstTerminal =
        coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError secondTerminal =
        coordinator.PublishInterruptVectorTableFailure(CpuId{2U}, ivtFailure);
    const Ia64ApplicationProcessorTakeoverPublicationError thirdOwned = coordinator.PublishOwnedPhysical(CpuId{3U});
    const Ia64ApplicationProcessorTakeoverPublicationError thirdTerminal =
        coordinator.PublishPalCaptureFailure(CpuId{3U}, palFailure);

    const auto firstSnapshot = coordinator.GetProcessorSnapshot(CpuId{1U});
    const auto secondSnapshot = coordinator.GetProcessorSnapshot(CpuId{2U});
    const auto thirdSnapshot = coordinator.GetProcessorSnapshot(CpuId{3U});
    const Ia64ApplicationProcessorTakeoverProgressSnapshot progress = coordinator.GetProgressSnapshot();
    bool terminalPayloadsExact{firstSnapshot.HasValue() && secondSnapshot.HasValue() && thirdSnapshot.HasValue()};
    if (terminalPayloadsExact)
    {
        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& success = firstSnapshot.GetValue();
        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& ivtFailed = secondSnapshot.GetValue();
        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& palFailed = thirdSnapshot.GetValue();
        terminalPayloadsExact =
            (success.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled) &&
            BytesEqual(success.PalCapabilities, capabilities) && FailureIsZero(success.Failure) &&
            (ivtFailed.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed) &&
            CapabilitiesAreZero(ivtFailed.PalCapabilities) &&
            (ivtFailed.Failure.Phase ==
             Ia64ApplicationProcessorTakeoverFailurePhase::InterruptVectorTableInstallation) &&
            BytesEqual(ivtFailed.Failure.InterruptVectorTableInstallationStatus, ivtFailure) &&
            BytesEqual(ivtFailed.Failure.PalCapabilityCapture, PalBootstrapCapabilityCaptureError{}) &&
            (ivtFailed.Failure.InitialTranslationInstallationError == InitialTranslationInstallError::None) &&
            (palFailed.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed) &&
            CapabilitiesAreZero(palFailed.PalCapabilities) &&
            (palFailed.Failure.Phase == Ia64ApplicationProcessorTakeoverFailurePhase::PalCapabilityCapture) &&
            BytesEqual(palFailed.Failure.PalCapabilityCapture, palFailure) &&
            BytesEqual(palFailed.Failure.InterruptVectorTableInstallationStatus,
                       InitialInterruptVectorTableApInstallStatus{}) &&
            (palFailed.Failure.InitialTranslationInstallationError == InitialTranslationInstallError::None);
    }
    const bool intermediatePalVisible =
        firstPalSnapshot.HasValue() &&
        (firstPalSnapshot.GetValue().Lifecycle ==
         Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished) &&
        BytesEqual(firstPalSnapshot.GetValue().PalCapabilities, capabilities) &&
        FailureIsZero(firstPalSnapshot.GetValue().Failure) &&
        (intermediateProgress.Processors[1U] == Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished) &&
        (intermediateProgress.TerminalMask == 0U) && (intermediateProgress.FailedMask == 0U);
    ASTERIA_EXPECT(runner, "PAL publication stays visible and only final success/IVT/PAL failure paths become terminal",
                   firstOwned == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   firstPalPublished == Ia64ApplicationProcessorTakeoverPublicationError::None, intermediatePalVisible,
                   firstTerminal == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   secondTerminal == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   thirdOwned == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   thirdTerminal == Ia64ApplicationProcessorTakeoverPublicationError::None, terminalPayloadsExact,
                   progress.Processors[1U] == Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled,
                   progress.Processors[2U] == Ia64ApplicationProcessorTakeoverLifecycle::Failed,
                   progress.Processors[3U] == Ia64ApplicationProcessorTakeoverLifecycle::Failed,
                   progress.TerminalMask == 0x0EU, progress.FailedMask == 0x0CU);
}

void TestInitialTranslationFailurePayload(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const PalBootstrapCapabilities capabilities = MakeCapabilities(0x191U);
    const bool setup =
        PrepareOwner(owner, allocator, 2U) &&
        (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None) &&
        coordinator.Claim(CpuId{1U}).HasValue() &&
        (coordinator.PublishOwnedPhysical(CpuId{1U}) == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
        (coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities) ==
         Ia64ApplicationProcessorTakeoverPublicationError::None);

    const Ia64ApplicationProcessorTakeoverPublicationError noneRejected =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, InitialTranslationInstallError::None);
    const Ia64ApplicationProcessorTakeoverPublicationError unknownRejected =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, static_cast<InitialTranslationInstallError>(9U));
    const auto intermediate = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverProgressSnapshot intermediateProgress = coordinator.GetProgressSnapshot();
    constexpr InitialTranslationInstallError translationFailure{InitialTranslationInstallError::InvalidResourceEntry};
    const Ia64ApplicationProcessorTakeoverPublicationError failurePublished =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, translationFailure);
    const Ia64ApplicationProcessorTakeoverPublicationError successAfterFailure =
        coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
    const auto terminal = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverProgressSnapshot terminalProgress = coordinator.GetProgressSnapshot();

    bool payloadExact{intermediate.HasValue() && terminal.HasValue()};
    if (payloadExact)
    {
        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& before = intermediate.GetValue();
        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& failed = terminal.GetValue();
        payloadExact =
            (before.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished) &&
            BytesEqual(before.PalCapabilities, capabilities) && FailureIsZero(before.Failure) &&
            (failed.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed) &&
            BytesEqual(failed.PalCapabilities, capabilities) &&
            (failed.Failure.Phase == Ia64ApplicationProcessorTakeoverFailurePhase::InitialTranslationInstallation) &&
            (failed.Failure.InterruptVectorTableInstallationStatus ==
             InitialInterruptVectorTableApInstallStatus::Installed) &&
            BytesEqual(failed.Failure.PalCapabilityCapture, PalBootstrapCapabilityCaptureError{}) &&
            (failed.Failure.InitialTranslationInstallationError == translationFailure);
    }
    ASTERIA_EXPECT(runner, "initial-translation failure keeps PAL data and selects only its typed error payload", setup,
                   noneRejected == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
                   unknownRejected == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
                   intermediateProgress.TerminalMask == 0U, intermediateProgress.FailedMask == 0U,
                   failurePublished == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   successAfterFailure == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
                   payloadExact, terminalProgress.TerminalMask == Foundation::UInt8{0x02U},
                   terminalProgress.FailedMask == Foundation::UInt8{0x02U});
}

void TestRseInformationFailurePayload(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const bool setup =
        PrepareOwner(owner, allocator, 3U) &&
        (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None) &&
        coordinator.Claim(CpuId{1U}).HasValue() &&
        (coordinator.PublishOwnedPhysical(CpuId{1U}) == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
        coordinator.Claim(CpuId{2U}).HasValue() &&
        (coordinator.PublishOwnedPhysical(CpuId{2U}) == Ia64ApplicationProcessorTakeoverPublicationError::None);
    const PalBootstrapCapabilityCaptureError decoding{
        .Phase = PalBootstrapCapabilityCapturePhase::RseInformationDecoding,
        .AdapterError = PalStaticCallAdapterError::None,
        .VirtualMemoryError = PalVirtualMemoryDecodeError::None,
        .TranslationCachePurgeError = PalTranslationCachePurgeDecodeError::None,
    };
    PalBootstrapCapabilityCaptureError inconsistent{decoding};
    inconsistent.AdapterError = PalStaticCallAdapterError::InvalidBinding;
    PalBootstrapCapabilityCaptureError invocation{decoding};
    invocation.Phase = PalBootstrapCapabilityCapturePhase::RseInformationInvocation;
    invocation.AdapterError = PalStaticCallAdapterError::UnsupportedProcessorState;
    // An invocation failure never reached a decoder.
    PalBootstrapCapabilityCaptureError decodedInvocation{invocation};
    decodedInvocation.VirtualMemoryError = PalVirtualMemoryDecodeError::SummaryCallFailed;

    const Ia64ApplicationProcessorTakeoverPublicationError inconsistentRejected =
        coordinator.PublishPalCaptureFailure(CpuId{1U}, inconsistent);
    const Ia64ApplicationProcessorTakeoverPublicationError decodingPublished =
        coordinator.PublishPalCaptureFailure(CpuId{1U}, decoding);
    const Ia64ApplicationProcessorTakeoverPublicationError decodedInvocationRejected =
        coordinator.PublishPalCaptureFailure(CpuId{2U}, decodedInvocation);
    const Ia64ApplicationProcessorTakeoverPublicationError invocationPublished =
        coordinator.PublishPalCaptureFailure(CpuId{2U}, invocation);
    const auto first = coordinator.GetProcessorSnapshot(CpuId{1U});
    const auto second = coordinator.GetProcessorSnapshot(CpuId{2U});
    ASTERIA_EXPECT(runner, "an AP's PAL_RSE_INFO rejection is a valid terminal PAL-capture failure payload", setup,
                   inconsistentRejected == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
                   decodingPublished == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   decodedInvocationRejected == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
                   invocationPublished == Ia64ApplicationProcessorTakeoverPublicationError::None, first.HasValue(),
                   first.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed,
                   BytesEqual(first.GetValue().Failure.PalCapabilityCapture, decoding), second.HasValue(),
                   second.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed,
                   BytesEqual(second.GetValue().Failure.PalCapabilityCapture, invocation));
}

void TestIllegalTransitionsAreAtomic(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const bool setup = PrepareOwner(owner, allocator, 4U) &&
                       (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None);
    const PalBootstrapCapabilities capabilities = MakeCapabilities(0x222U);
    const PalBootstrapCapabilityCaptureError palFailure = MakePalFailure();
    constexpr InitialInterruptVectorTableApInstallStatus ivtFailure{
        InitialInterruptVectorTableApInstallStatus::InvalidSummary};
    constexpr InitialTranslationInstallError translationFailure{InitialTranslationInstallError::InvalidCapabilities};

    const Ia64ApplicationProcessorTakeoverPublicationError earlyOwned = coordinator.PublishOwnedPhysical(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError earlyPalSuccess =
        coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities);
    const Ia64ApplicationProcessorTakeoverPublicationError earlyPalFailure =
        coordinator.PublishPalCaptureFailure(CpuId{1U}, palFailure);
    const Ia64ApplicationProcessorTakeoverPublicationError earlyIvtFailure =
        coordinator.PublishInterruptVectorTableFailure(CpuId{1U}, ivtFailure);
    const Ia64ApplicationProcessorTakeoverPublicationError earlyTranslationSuccess =
        coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError earlyTranslationFailure =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, translationFailure);
    const auto stillPrepared = coordinator.GetProcessorSnapshot(CpuId{1U});

    const auto firstClaim = coordinator.Claim(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError palBeforeOwned =
        coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities);
    const auto stillClaimed = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError firstOwned = coordinator.PublishOwnedPhysical(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError duplicateOwned = coordinator.PublishOwnedPhysical(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError ivtAfterOwned =
        coordinator.PublishInterruptVectorTableFailure(CpuId{1U}, ivtFailure);
    const Ia64ApplicationProcessorTakeoverPublicationError translationSuccessBeforePal =
        coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError translationFailureBeforePal =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, translationFailure);
    const auto stillOwned = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError firstPalPublished =
        coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities);
    const Ia64ApplicationProcessorTakeoverPublicationError repeatPalFailure =
        coordinator.PublishPalCaptureFailure(CpuId{1U}, palFailure);
    const Ia64ApplicationProcessorTakeoverPublicationError noneTranslationFailure =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, InitialTranslationInstallError::None);
    const auto stillPalPublished = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverProgressSnapshot palPublishedProgress = coordinator.GetProgressSnapshot();
    const Ia64ApplicationProcessorTakeoverPublicationError firstTerminal =
        coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverPublicationError repeatTerminal =
        coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, translationFailure);
    const auto terminalSuccess = coordinator.GetProcessorSnapshot(CpuId{1U});

    const auto secondClaim = coordinator.Claim(CpuId{2U});
    const Ia64ApplicationProcessorTakeoverPublicationError invalidIvtPayload =
        coordinator.PublishInterruptVectorTableFailure(CpuId{2U},
                                                       InitialInterruptVectorTableApInstallStatus::Installed);
    const Ia64ApplicationProcessorTakeoverPublicationError invalidIvtValue =
        coordinator.PublishInterruptVectorTableFailure(CpuId{2U},
                                                       static_cast<InitialInterruptVectorTableApInstallStatus>(4U));
    const auto secondStillClaimed = coordinator.GetProcessorSnapshot(CpuId{2U});

    const auto thirdClaim = coordinator.Claim(CpuId{3U});
    const Ia64ApplicationProcessorTakeoverPublicationError thirdOwned = coordinator.PublishOwnedPhysical(CpuId{3U});
    const PalBootstrapCapabilityCaptureError emptyPalFailure{};
    const Ia64ApplicationProcessorTakeoverPublicationError invalidPalPayload =
        coordinator.PublishPalCaptureFailure(CpuId{3U}, emptyPalFailure);
    PalBootstrapCapabilityCaptureError inconsistentPalFailure = MakePalFailure();
    inconsistentPalFailure.AdapterError = PalStaticCallAdapterError::InvalidBinding;
    const Ia64ApplicationProcessorTakeoverPublicationError inconsistentPalPayload =
        coordinator.PublishPalCaptureFailure(CpuId{3U}, inconsistentPalFailure);
    const auto thirdStillOwned = coordinator.GetProcessorSnapshot(CpuId{3U});

    const bool intermediateStatesExact =
        stillPrepared.HasValue() &&
        (stillPrepared.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Prepared) &&
        SnapshotPayloadIsZero(stillPrepared.GetValue()) && stillClaimed.HasValue() &&
        (stillClaimed.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Claimed) &&
        SnapshotPayloadIsZero(stillClaimed.GetValue()) && stillOwned.HasValue() &&
        (stillOwned.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::OwnedPhysical) &&
        SnapshotPayloadIsZero(stillOwned.GetValue()) && secondStillClaimed.HasValue() &&
        (secondStillClaimed.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Claimed) &&
        SnapshotPayloadIsZero(secondStillClaimed.GetValue()) && thirdStillOwned.HasValue() &&
        (thirdStillOwned.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::OwnedPhysical) &&
        SnapshotPayloadIsZero(thirdStillOwned.GetValue()) && stillPalPublished.HasValue() &&
        (stillPalPublished.GetValue().Lifecycle ==
         Ia64ApplicationProcessorTakeoverLifecycle::PalCapabilitiesPublished) &&
        BytesEqual(stillPalPublished.GetValue().PalCapabilities, capabilities) &&
        FailureIsZero(stillPalPublished.GetValue().Failure) && (palPublishedProgress.TerminalMask == 0U) &&
        terminalSuccess.HasValue() &&
        (terminalSuccess.GetValue().Lifecycle ==
         Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled) &&
        BytesEqual(terminalSuccess.GetValue().PalCapabilities, capabilities) &&
        FailureIsZero(terminalSuccess.GetValue().Failure);

    ASTERIA_EXPECT(
        runner, "illegal transitions and invalid failure payloads mutate no slot data", setup,
        earlyOwned == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        earlyPalSuccess == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        earlyPalFailure == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        earlyIvtFailure == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        earlyTranslationSuccess == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        earlyTranslationFailure == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        firstClaim.HasValue(),
        palBeforeOwned == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        firstOwned == Ia64ApplicationProcessorTakeoverPublicationError::None,
        duplicateOwned == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        ivtAfterOwned == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        translationSuccessBeforePal == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        translationFailureBeforePal == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        firstPalPublished == Ia64ApplicationProcessorTakeoverPublicationError::None,
        repeatPalFailure == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        noneTranslationFailure == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
        firstTerminal == Ia64ApplicationProcessorTakeoverPublicationError::None,
        repeatTerminal == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState,
        secondClaim.HasValue(),
        invalidIvtPayload == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
        invalidIvtValue == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
        thirdClaim.HasValue(), thirdOwned == Ia64ApplicationProcessorTakeoverPublicationError::None,
        invalidPalPayload == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
        inconsistentPalPayload == Ia64ApplicationProcessorTakeoverPublicationError::InvalidFailurePayload,
        intermediateStatesExact);

    const Ia64ApplicationProcessorTakeoverPublicationError unknownOwned = coordinator.PublishOwnedPhysical(CpuId{});
    const Ia64ApplicationProcessorTakeoverPublicationError bspOwned = coordinator.PublishOwnedPhysical(CpuId{0U});
    ASTERIA_EXPECT(runner, "wrong and BSP publication CpuIds are rejected without mutation",
                   unknownOwned == Ia64ApplicationProcessorTakeoverPublicationError::UnknownProcessor,
                   bspOwned == Ia64ApplicationProcessorTakeoverPublicationError::BootstrapProcessor);
}

void TestConcurrentDuplicateClaim(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const bool setup = PrepareOwner(owner, allocator, 2U) &&
                       (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None);

    std::atomic<Foundation::UInt8> readyCount{0U};
    std::atomic<bool> start{false};
    bool firstClaimed{false};
    bool secondClaimed{false};
    Ia64ApplicationProcessorTakeoverClaimError firstError{Ia64ApplicationProcessorTakeoverClaimError::None};
    Ia64ApplicationProcessorTakeoverClaimError secondError{Ia64ApplicationProcessorTakeoverClaimError::None};
    const Ia64ApplicationProcessorTakeoverRecord* firstRecord{nullptr};
    const Ia64ApplicationProcessorTakeoverRecord* secondRecord{nullptr};
    const auto worker =
        [&coordinator, &readyCount, &start](bool& claimed, Ia64ApplicationProcessorTakeoverClaimError& error,
                                            const Ia64ApplicationProcessorTakeoverRecord*& record) noexcept
    {
        static_cast<void>(readyCount.fetch_add(1U));
        while (!start.load())
        {
            std::this_thread::yield();
        }
        const auto result = coordinator.Claim(CpuId{1U});
        claimed = result.HasValue();
        error = result.GetError();
        if (claimed)
        {
            record = result.GetValue();
        }
    };

    std::thread first{[&]() noexcept
                      {
                          worker(firstClaimed, firstError, firstRecord);
                      }};
    std::thread second{[&]() noexcept
                       {
                           worker(secondClaimed, secondError, secondRecord);
                       }};
    while (readyCount.load() != 2U)
    {
        std::this_thread::yield();
    }
    start.store(true);
    first.join();
    second.join();

    const auto snapshot = coordinator.GetProcessorSnapshot(CpuId{1U});
    const bool firstWon = firstClaimed && (firstError == Ia64ApplicationProcessorTakeoverClaimError::None) &&
                          (firstRecord != nullptr) && !secondClaimed &&
                          (secondError == Ia64ApplicationProcessorTakeoverClaimError::DuplicateClaim);
    const bool secondWon = secondClaimed && (secondError == Ia64ApplicationProcessorTakeoverClaimError::None) &&
                           (secondRecord != nullptr) && !firstClaimed &&
                           (firstError == Ia64ApplicationProcessorTakeoverClaimError::DuplicateClaim);
    ASTERIA_EXPECT(runner, "simultaneous claims produce exactly one stable record owner", setup, firstWon || secondWon,
                   snapshot.HasValue(),
                   snapshot.GetValue().Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Claimed);
}

void TestAcquireSafeConcurrentPublications(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const PalBootstrapCapabilities capabilities = MakeCapabilities(0x333U);
    const bool setup =
        PrepareOwner(owner, allocator, 4U) &&
        (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None) &&
        coordinator.Claim(CpuId{1U}).HasValue() && coordinator.Claim(CpuId{2U}).HasValue() &&
        coordinator.Claim(CpuId{3U}).HasValue() &&
        (coordinator.PublishOwnedPhysical(CpuId{1U}) == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
        (coordinator.PublishOwnedPhysical(CpuId{3U}) == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
        (coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities) ==
         Ia64ApplicationProcessorTakeoverPublicationError::None) &&
        (coordinator.PublishPalCaptureSuccess(CpuId{3U}, capabilities) ==
         Ia64ApplicationProcessorTakeoverPublicationError::None);
    constexpr InitialInterruptVectorTableApInstallStatus ivtFailure{
        InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState};
    constexpr InitialTranslationInstallError translationFailure{
        InitialTranslationInstallError::UnsupportedProcessorState};
    constexpr Foundation::Size observationLimit{100'000U};
    constexpr Foundation::UInt8 publisherCount{3U};

    bool initiallyTerminal{false};
    const bool initialSnapshotsValid = AreConcurrentPublicationSnapshotsValid(coordinator, capabilities, ivtFailure,
                                                                              translationFailure, initiallyTerminal) &&
                                       !initiallyTerminal;

    std::atomic<bool> readerReady{false};
    std::atomic<Foundation::UInt8> publisherCompletionCount{0U};
    bool readerValid{true};
    Ia64ApplicationProcessorTakeoverPublicationError successResult{
        Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState};
    Ia64ApplicationProcessorTakeoverPublicationError ivtResult{
        Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState};
    Ia64ApplicationProcessorTakeoverPublicationError translationResult{
        Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState};

    std::thread reader{
        [&]() noexcept
        {
            readerReady.store(true, std::memory_order_release);
            for (Foundation::Size attempt{0U}; attempt < observationLimit; ++attempt)
            {
                bool allTerminal{false};
                readerValid =
                    readerValid && AreConcurrentPublicationSnapshotsValid(coordinator, capabilities, ivtFailure,
                                                                          translationFailure, allTerminal);
                if (publisherCompletionCount.load(std::memory_order_acquire) == publisherCount)
                {
                    bool acquiredAllTerminal{false};
                    readerValid = readerValid &&
                                  AreConcurrentPublicationSnapshotsValid(coordinator, capabilities, ivtFailure,
                                                                         translationFailure, acquiredAllTerminal) &&
                                  acquiredAllTerminal;
                    break;
                }
                std::this_thread::yield();
            }
            // Budget expiry only ends concurrent sampling. The main thread checks
            // completion and performs the final acquire observations after joins.
        }};
    const auto waitForReader = [&readerReady]() noexcept
    {
        for (Foundation::Size attempt{0U}; attempt < observationLimit; ++attempt)
        {
            if (readerReady.load(std::memory_order_acquire))
            {
                return;
            }
            std::this_thread::yield();
        }
        // Synchronous initial validation already covered the prepublication
        // state, so a starved reader must not indefinitely block publishers.
    };
    std::thread successPublisher{
        [&]() noexcept
        {
            waitForReader();
            successResult = coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
            static_cast<void>(publisherCompletionCount.fetch_add(Foundation::UInt8{1U}, std::memory_order_release));
        }};
    std::thread ivtPublisher{
        [&]() noexcept
        {
            waitForReader();
            ivtResult = coordinator.PublishInterruptVectorTableFailure(CpuId{2U}, ivtFailure);
            static_cast<void>(publisherCompletionCount.fetch_add(Foundation::UInt8{1U}, std::memory_order_release));
        }};
    std::thread translationPublisher{
        [&]() noexcept
        {
            waitForReader();
            translationResult = coordinator.PublishInitialTranslationInstallFailure(CpuId{3U}, translationFailure);
            static_cast<void>(publisherCompletionCount.fetch_add(Foundation::UInt8{1U}, std::memory_order_release));
        }};
    successPublisher.join();
    ivtPublisher.join();
    translationPublisher.join();
    reader.join();

    const bool everyPublisherCompleted = publisherCompletionCount.load(std::memory_order_acquire) == publisherCount;
    bool allTerminal{false};
    const bool finalSnapshotsValid =
        AreConcurrentPublicationSnapshotsValid(coordinator, capabilities, ivtFailure, translationFailure, allTerminal);
    const Ia64ApplicationProcessorTakeoverProgressSnapshot progress = coordinator.GetProgressSnapshot();
    ASTERIA_EXPECT(runner, "acquire snapshots retain PAL data and expose only complete final payloads", setup,
                   successResult == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   ivtResult == Ia64ApplicationProcessorTakeoverPublicationError::None,
                   translationResult == Ia64ApplicationProcessorTakeoverPublicationError::None, initialSnapshotsValid,
                   everyPublisherCompleted, readerValid, finalSnapshotsValid, allTerminal,
                   progress.FailedMask == Foundation::UInt8{0x0CU});
}

void TestConcurrentInitialTranslationPublicationRace(TestContext& runner) noexcept
{
    BootstrapPageAllocator allocator;
    ApplicationProcessorExecutionStorageOwner owner;
    Ia64ApplicationProcessorTakeoverCoordinator coordinator;
    const PalBootstrapCapabilities capabilities = MakeCapabilities(0x444U);
    const bool setup =
        PrepareOwner(owner, allocator, 2U) &&
        (coordinator.Initialize(owner) == Ia64ApplicationProcessorTakeoverInitializationError::None) &&
        coordinator.Claim(CpuId{1U}).HasValue() &&
        (coordinator.PublishOwnedPhysical(CpuId{1U}) == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
        (coordinator.PublishPalCaptureSuccess(CpuId{1U}, capabilities) ==
         Ia64ApplicationProcessorTakeoverPublicationError::None);
    constexpr InitialTranslationInstallError translationFailure{
        InitialTranslationInstallError::InvalidFirmwareTranslation};

    std::atomic<Foundation::UInt8> readyCount{0U};
    std::atomic<bool> start{false};
    Ia64ApplicationProcessorTakeoverPublicationError successResult{
        Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState};
    Ia64ApplicationProcessorTakeoverPublicationError failureResult{
        Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState};
    const auto waitForStart = [&readyCount, &start]() noexcept
    {
        static_cast<void>(readyCount.fetch_add(1U));
        while (!start.load())
        {
            std::this_thread::yield();
        }
    };
    std::thread successPublisher{[&]() noexcept
                                 {
                                     waitForStart();
                                     successResult = coordinator.PublishInitialTranslationInstallSuccess(CpuId{1U});
                                 }};
    std::thread failurePublisher{
        [&]() noexcept
        {
            waitForStart();
            failureResult = coordinator.PublishInitialTranslationInstallFailure(CpuId{1U}, translationFailure);
        }};
    while (readyCount.load() != 2U)
    {
        std::this_thread::yield();
    }
    start.store(true);
    successPublisher.join();
    failurePublisher.join();

    const auto snapshot = coordinator.GetProcessorSnapshot(CpuId{1U});
    const Ia64ApplicationProcessorTakeoverProgressSnapshot progress = coordinator.GetProgressSnapshot();
    bool successWon{false};
    bool failureWon{false};
    if (snapshot.HasValue())
    {
        const Ia64ApplicationProcessorTakeoverProcessorSnapshot& terminal = snapshot.GetValue();
        successWon = (successResult == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
                     (failureResult == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState) &&
                     (terminal.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::InitialTranslationsInstalled) &&
                     BytesEqual(terminal.PalCapabilities, capabilities) && FailureIsZero(terminal.Failure) &&
                     (progress.TerminalMask == Foundation::UInt8{0x02U}) && (progress.FailedMask == 0U);
        failureWon =
            (failureResult == Ia64ApplicationProcessorTakeoverPublicationError::None) &&
            (successResult == Ia64ApplicationProcessorTakeoverPublicationError::InvalidPrecedingState) &&
            (terminal.Lifecycle == Ia64ApplicationProcessorTakeoverLifecycle::Failed) &&
            BytesEqual(terminal.PalCapabilities, capabilities) &&
            (terminal.Failure.Phase == Ia64ApplicationProcessorTakeoverFailurePhase::InitialTranslationInstallation) &&
            (terminal.Failure.InterruptVectorTableInstallationStatus ==
             InitialInterruptVectorTableApInstallStatus::Installed) &&
            BytesEqual(terminal.Failure.PalCapabilityCapture, PalBootstrapCapabilityCaptureError{}) &&
            (terminal.Failure.InitialTranslationInstallationError == translationFailure) &&
            (progress.TerminalMask == Foundation::UInt8{0x02U}) && (progress.FailedMask == Foundation::UInt8{0x02U});
    }
    ASTERIA_EXPECT(runner,
                   "same-slot initial-translation success/failure selects one exact "
                   "terminal outcome",
                   setup, successWon || failureWon);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64ApplicationProcessorTakeoverCoordinatorTests"};
    Asteria::Test::TestNativeSingletonAndNotReadyOwner(runner);
    Asteria::Test::TestFailedOwners(runner);
    Asteria::Test::TestProcessorShape(runner, 1U);
    Asteria::Test::TestProcessorShape(runner, 2U);
    Asteria::Test::TestProcessorShape(runner, 3U);
    Asteria::Test::TestProcessorShape(runner, 4U);
    Asteria::Test::TestClaimsAndLegalTransitions(runner);
    Asteria::Test::TestInitialTranslationFailurePayload(runner);
    Asteria::Test::TestRseInformationFailurePayload(runner);
    Asteria::Test::TestIllegalTransitionsAreAtomic(runner);
    Asteria::Test::TestConcurrentDuplicateClaim(runner);
    Asteria::Test::TestAcquireSafeConcurrentPublications(runner);
    Asteria::Test::TestConcurrentInitialTranslationPublicationRace(runner);
    return runner.Finish();
}
