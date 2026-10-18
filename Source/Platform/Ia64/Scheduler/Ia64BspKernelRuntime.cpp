// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Scheduler/Ia64BspKernelRuntime.hpp"
#include "Platform/Ia64/System/Ia64PagerRuntime.hpp"

#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Ia64/Diagnostics/Ia64StopStage.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Interrupt/Ia64DeviceInterruptPoll.hpp"
#include "Platform/Ia64/Scheduler/Ia64CrossProcessorWaitBudget.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

// The one-shot BSP native-preemption probe (its sentinel, coordinator, idle
// landing and the preparation and validation only they use) is compiled
// wherever the option is not configured, so the hosted tests and the
// compile checks keep covering it, and in a firmware built with
// ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=1. A production firmware, built with
// the option zero, contains none of it; Ia64BspKernelRuntime.S applies the
// same rule to the probe's assembly.
#if !defined(ASTERIA_IA64_NATIVE_PREEMPTION_PROBE) || (ASTERIA_IA64_NATIVE_PREEMPTION_PROBE != 0)
#define ASTERIA_IA64_BSP_NATIVE_PREEMPTION_PROBE_CODE 1
#else
#define ASTERIA_IA64_BSP_NATIVE_PREEMPTION_PROBE_CODE 0
#endif

// The synthetic user probe runs at PL3 with external interrupts masked, so its worker keeps one
// scheduling window that outlasts the boot, as do the hosted tests, which drive that probe. Every
// other firmware gives the initial thread a time slice.
#if defined(ASTERIA_TARGET_IA64) && defined(ASTERIA_IA64_SYNTHETIC_USER_PROBE) &&                                      \
    (ASTERIA_IA64_SYNTHETIC_USER_PROBE == 0)
#define ASTERIA_IA64_BSP_USER_TIME_SLICE 1
#else
#define ASTERIA_IA64_BSP_USER_TIME_SLICE 0
#endif

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
void* operator new(Asteria::Foundation::Size sizeBytes, void* storage) noexcept;
#else
#include <new>
#endif

extern "C" void AsteriaIa64CaptureBspKernelWorkerEvidence(
    Asteria::Platform::Ia64::Scheduler::Ia64BspKernelWorkerEvidence* evidence) noexcept;

extern "C" void AsteriaIa64InitializeBspKernelWorkerDescriptor(Asteria::Foundation::UInt64* descriptor) noexcept;

extern "C" void AsteriaIa64InitializeBspNativePreemptionDescriptor(Asteria::Foundation::UInt64* descriptor) noexcept;

extern "C" void AsteriaIa64CaptureBspNativePreemptionObservation(
    Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionSentinelObservation* observation) noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64RunBspNativePreemptionSentinel(
    Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionControl* control) noexcept;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64BspNativePreemptionCoordinator(void* argument) noexcept;

extern "C" [[noreturn]] void AsteriaIa64BspNativePreemptionIdleLanding(void* argument) noexcept;

// The target entry points are in Ia64BspKernelRuntime.S.
#if !defined(__ia64__)
extern "C" void AsteriaIa64CaptureBspKernelWorkerEvidence(
    Asteria::Platform::Ia64::Scheduler::Ia64BspKernelWorkerEvidence* const evidence) noexcept
{
    if (evidence != nullptr)
    {
        *evidence = Asteria::Platform::Ia64::Scheduler::Ia64BspKernelWorkerEvidence{
            .StackPointer = 0U,
            .RseBackingStorePointer = 0U,
            .RseBackingStoreBase = 0U,
            .RseNatCollection = 0U,
            .RseConfiguration = 0U,
            .ProcessorStatus = 0U,
            .CaptureComplete = 1U,
            .Reserved = {},
        };
    }
}

extern "C" void AsteriaIa64InitializeBspKernelWorkerDescriptor(Asteria::Foundation::UInt64*) noexcept
{
}

extern "C" void AsteriaIa64InitializeBspNativePreemptionDescriptor(Asteria::Foundation::UInt64*) noexcept
{
}

extern "C" void AsteriaIa64CaptureBspNativePreemptionObservation(
    Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionSentinelObservation* const observation) noexcept
{
    if (observation != nullptr)
    {
        *observation = {};
    }
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64RunBspNativePreemptionSentinel(Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionControl*) noexcept
{
    return 1U;
}

extern "C" [[noreturn]] void AsteriaIa64KernelIdle() noexcept
{
    __builtin_trap();
}
#endif

namespace Asteria::Platform::Ia64::Scheduler
{
namespace
{
using Controller = Kernel::Scheduler::ProcessorRunController;
using DispatchResult = Foundation::Result<Ia64BspKernelRuntimeDispatchSummary, Ia64BspKernelRuntimeError>;
using PreparationResult = Foundation::Result<Ia64BspKernelRuntimePreparationSummary, Ia64BspKernelRuntimeError>;
using UserProbeDispatchResult =
    Foundation::Result<Ia64BspKernelRuntimeUserProbeDispatchSummary, Ia64BspKernelRuntimeUserProbeError>;
using UserProbePreparationResult =
    Foundation::Result<Ia64BspKernelRuntimeUserProbePreparationSummary, Ia64BspKernelRuntimeUserProbeError>;
using InitialUserImageDispatchResult =
    Foundation::Result<Ia64BspKernelRuntimeInitialUserImageDispatchSummary, Ia64BspKernelRuntimeInitialUserImageError>;
using InitialUserImagePreparationResult = Foundation::Result<Ia64BspKernelRuntimeInitialUserImagePreparationSummary,
                                                             Ia64BspKernelRuntimeInitialUserImageError>;
using LocalTimerProbeResult =
    Foundation::Result<Ia64BspKernelRuntimeLocalTimerProbeSummary, Ia64BspKernelRuntimeLocalTimerProbeError>;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
// Late deadlines the initial-image idle loop retries in a row while no PL3
// timer tick shows that a thread ran (DispatchInitialUserImage).
constexpr Foundation::Size LateDispatchAttempts{64U};
// How far past a replenishment deadline idle polls the ITC before it gives up.
constexpr Foundation::UInt64 ReplenishmentWaitGraceTicks{4'000'000'000U};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 PageOffsetMask{Kernel::Memory::MappingPageSizeBytes - 1U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalFirmwareMemoryType{7U};
constexpr Foundation::UInt64 TranslationStateMask{(Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{1U} << 27U) |
                                                  (Foundation::UInt64{1U} << 17U)};
constexpr Foundation::UInt64 InterruptionEnableMask{Foundation::UInt64{1U} << 14U};
constexpr Kernel::Memory::PagePermissions UserStackPermissions{
    Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}
        .With(Kernel::Memory::PagePermission::Write)
        .With(Kernel::Memory::PagePermission::User)};
constexpr Kernel::Ipc::EndpointConfiguration UserProbeEndpointConfiguration{
    .MaximumQueuedBytes = 88U,
    .ReservedControlBytes = 0U,
    .MaximumBytesPerSender = 88U,
    .MaximumQueuedMessages = 1U,
    .ReservedControlMessages = 0U,
    .MaximumMessagesPerSender = 1U,
    .MaximumQueuedAttachments = 0U,
    .ReservedControlAttachments = 0U,
    .MaximumAttachmentsPerSender = 0U,
    .Reserved = 0U,
};
constexpr Kernel::Ipc::EndpointConfiguration InitialUserImageEndpointConfiguration{
    .MaximumQueuedBytes = 88U,
    .ReservedControlBytes = 0U,
    .MaximumBytesPerSender = 88U,
    .MaximumQueuedMessages = 1U,
    .ReservedControlMessages = 0U,
    .MaximumMessagesPerSender = 1U,
    .MaximumQueuedAttachments = 0U,
    .ReservedControlAttachments = 0U,
    .MaximumAttachmentsPerSender = 0U,
    .Reserved = 0U,
};
constexpr Kernel::Object::CapabilityRights UserProbeEndpointRights{
    Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke}.With(
        Kernel::Object::CapabilityRight::Read)};
constexpr Kernel::Object::CapabilityRights InitialUserImageEndpointRights{
    Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Inspect}
        .With(Kernel::Object::CapabilityRight::Invoke)
        .With(Kernel::Object::CapabilityRight::Read)};
// Write is the channel's only operation; Inspect lets the owner and the image
// Query the capability, as on AMD64.
constexpr Kernel::Object::CapabilityRights InitialUserImageDiagnosticChannelRights{
    Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Inspect}.With(
        Kernel::Object::CapabilityRight::Write)};
// The boot Endpoint and DiagnosticChannel metadata, their two capabilities,
// and the Endpoint's queue.
constexpr Kernel::Resource::ResourceReservation InitialUserImageResourceReservation{
    .CommittedMemoryPages = 0U,
    .ResidentMemoryPages = 0U,
    .KernelMetadataBytes = sizeof(Kernel::Ipc::Endpoint) + sizeof(Kernel::Diagnostics::DiagnosticChannel),
    .CapabilityHandles = 2U,
    .IpcQueueBytes = InitialUserImageEndpointConfiguration.MaximumQueuedBytes,
    .Threads = 0U,
};
constexpr Kernel::Object::CapabilityRights UserProbeWaitSetRights{Kernel::Object::CapabilityRight::Wait};
constexpr Foundation::UInt8 UserProbeCanonicalFrame[Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes]{
    0x41U, 0x49U, 0x50U, 0x43U, 0x01U, 0x00U, 0x58U, 0x00U, 0x58U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

static_assert(UserProbeEndpointConfiguration.MaximumQueuedBytes == 88U);
static_assert(UserProbeEndpointConfiguration.ReservedControlBytes == 0U);
static_assert(UserProbeEndpointConfiguration.MaximumBytesPerSender == 88U);
static_assert(UserProbeEndpointConfiguration.MaximumQueuedMessages == 1U);
static_assert(UserProbeEndpointConfiguration.ReservedControlMessages == 0U);
static_assert(UserProbeEndpointConfiguration.MaximumMessagesPerSender == 1U);
static_assert(UserProbeEndpointConfiguration.MaximumQueuedAttachments == 0U);
static_assert(UserProbeEndpointConfiguration.ReservedControlAttachments == 0U);
static_assert(UserProbeEndpointConfiguration.MaximumAttachmentsPerSender == 0U);
static_assert(UserProbeEndpointConfiguration.Reserved == 0U);
static_assert(InitialUserImageEndpointConfiguration.MaximumQueuedBytes == 88U);
static_assert(InitialUserImageEndpointConfiguration.ReservedControlBytes == 0U);
static_assert(InitialUserImageEndpointConfiguration.MaximumBytesPerSender == 88U);
static_assert(InitialUserImageEndpointConfiguration.MaximumQueuedMessages == 1U);
static_assert(InitialUserImageEndpointConfiguration.ReservedControlMessages == 0U);
static_assert(InitialUserImageEndpointConfiguration.MaximumMessagesPerSender == 1U);
static_assert(InitialUserImageEndpointConfiguration.MaximumQueuedAttachments == 0U);
static_assert(InitialUserImageEndpointConfiguration.ReservedControlAttachments == 0U);
static_assert(InitialUserImageEndpointConfiguration.MaximumAttachmentsPerSender == 0U);
static_assert(InitialUserImageEndpointConfiguration.Reserved == 0U);
static_assert(UserProbeEndpointRights.ContainsAll(Kernel::Object::CapabilityRights{
    Kernel::Object::CapabilityRight::Invoke}));
static_assert(UserProbeEndpointRights.ContainsAll(Kernel::Object::CapabilityRights{
    Kernel::Object::CapabilityRight::Read}));
static_assert(UserProbeEndpointRights.GetRawValue() == 6U);
static_assert(InitialUserImageEndpointRights.GetRawValue() == 7U);
static_assert(InitialUserImageDiagnosticChannelRights.GetRawValue() == 9U);
static_assert(InitialUserImageResourceReservation.KernelMetadataBytes ==
              sizeof(Kernel::Ipc::Endpoint) + sizeof(Kernel::Diagnostics::DiagnosticChannel));
static_assert(InitialUserImageResourceReservation.CapabilityHandles == 2U);
static_assert(InitialUserImageResourceReservation.IpcQueueBytes == 88U);
static_assert(UserProbeWaitSetRights.GetRawValue() == 32U);
static_assert(sizeof(UserProbeCanonicalFrame) == Kernel::Ipc::MessageCodec::WireHeaderSizeBytes);
static_assert(Ia64UserExecutionProbeIpcBootstrapVirtualAddress >= Ia64UserExecutionProbeMemoryStackVirtualStart);
static_assert(Ia64UserExecutionProbeIpcBootstrapVirtualAddress + Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes <=
              Ia64UserExecutionProbeMemoryStackVirtualStart + Ia64UserExecutionProbeMemoryStackSizeBytes);

[[nodiscard]] constexpr Foundation::UInt64 AddressOf(const void* const storage) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(storage));
}

[[nodiscard]] constexpr bool TryGetEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                       Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

void WriteLittleEndian64(Foundation::UInt8* const destination, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(value); ++index)
    {
        destination[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] bool IsLittleEndian64(const Foundation::UInt8* const encoded, const Foundation::UInt64 expected) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(expected); ++index)
    {
        if (encoded[index] != static_cast<Foundation::UInt8>(expected >> (index * 8U)))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool BytesAreEqual(const Foundation::UInt8* const first, const Foundation::UInt8* const second,
                                 const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool BytesAreZero(const Foundation::UInt8* const bytes,
                                          const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr Foundation::UInt8 ProcessorBit(const Kernel::Scheduler::CpuId processor) noexcept
{
    return static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << processor.GetValue());
}

[[nodiscard]] constexpr bool TryGetPageRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                                Foundation::UInt64& end) noexcept
{
    if ((pages == 0U) || (pages > (MaximumUInt64 / Kernel::Memory::MappingPageSizeBytes)))
    {
        return false;
    }
    return TryGetEnd(start, pages * Kernel::Memory::MappingPageSizeBytes, end);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool
InitialUserImagePlansMatch(const Executable::Ia64InitialUserImageMemoryRangePlan& first,
                           const Executable::Ia64InitialUserImageMemoryRangePlan& second) noexcept
{
    return (first.Purpose == second.Purpose) && (first.GuardPolicy == second.GuardPolicy) &&
           (first.Permissions.GetBits() == second.Permissions.GetBits()) &&
           BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved)) &&
           (first.OriginalSegmentIndex == second.OriginalSegmentIndex) && (first.VirtualStart == second.VirtualStart) &&
           (first.Pages.GetValue() == second.Pages.GetValue()) && (first.FrameOffsetPages == second.FrameOffsetPages) &&
           first.MemoryObject.IsEqualTo(second.MemoryObject);
}

[[nodiscard]] constexpr bool
InitialUserImageReceiptMatches(const Kernel::Memory::AddressSpaceMappingReceipt& receipt,
                               const Executable::Ia64InitialUserImageMemoryRangePlan& plan,
                               const Executable::Ia64InitialUserImageMappingSummary& summary,
                               const Foundation::Size rangeIndex) noexcept
{
    return (receipt.MappingSequence == summary.FirstMappingSequence + rangeIndex) &&
           (receipt.MappingCount == summary.InitialMappingCount + rangeIndex + 1U) &&
           (receipt.VirtualStart.GetValue() == plan.VirtualStart) &&
           (receipt.Pages.GetValue() == plan.Pages.GetValue());
}

[[nodiscard]] constexpr bool
InitialUserImageMappingRecordMatches(const Kernel::Memory::AddressSpaceMappingRecord& record,
                                     const Executable::Ia64InitialUserImageMemoryRangePlan& plan,
                                     const Executable::Ia64InitialUserImageMemorySummary& summary) noexcept
{
    if (plan.FrameOffsetPages > (MaximumUInt64 / Kernel::Memory::MappingPageSizeBytes))
    {
        return false;
    }
    const Foundation::UInt64 physicalOffset = plan.FrameOffsetPages * Kernel::Memory::MappingPageSizeBytes;
    if (summary.AggregateArena.PhysicalStart > (MaximumUInt64 - physicalOffset))
    {
        return false;
    }
    return record.MemoryObject.IsEqualTo(plan.MemoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(summary.AggregateFrame) && (record.VirtualStart.GetValue() == plan.VirtualStart) &&
           (record.PhysicalStart.GetValue() == summary.AggregateArena.PhysicalStart + physicalOffset) &&
           (record.Pages.GetValue() == plan.Pages.GetValue()) && (record.MemoryObjectOffsetPages == 0U) &&
           (record.Permissions.GetBits() == plan.Permissions.GetBits()) &&
           (record.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (record.Kind == Kernel::Memory::FrameKind::Ram) &&
           (record.Commitment == Kernel::Memory::MemoryCommitment::Pinned) && record.RuntimeMapping &&
           BytesAreZero(record.Reserved, sizeof(record.Reserved));
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions SupervisorReadWritePermissions() noexcept
{
    return Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
        Kernel::Memory::PagePermission::Write);
}

[[nodiscard]] constexpr bool ResourceUsageIsBounded(const Kernel::Resource::ResourceDomainSnapshot& snapshot) noexcept
{
    return (snapshot.Usage.CommittedMemoryPages <= snapshot.Limits.CommittedMemoryPages) &&
           (snapshot.Usage.ResidentMemoryPages <= snapshot.Limits.ResidentMemoryPages) &&
           (snapshot.Usage.KernelMetadataBytes <= snapshot.Limits.KernelMetadataBytes) &&
           (snapshot.Usage.CapabilityHandles <= snapshot.Limits.CapabilityHandles) &&
           (snapshot.Usage.IpcQueueBytes <= snapshot.Limits.IpcQueueBytes) &&
           (snapshot.Usage.Threads <= snapshot.Limits.Threads) &&
           (snapshot.Usage.ChildDomains <= snapshot.Limits.ChildDomains);
}

[[nodiscard]] constexpr bool
ResourceCapacityIsSufficient(const Kernel::Resource::ResourceDomainSnapshot& snapshot) noexcept
{
    return ResourceUsageIsBounded(snapshot) &&
           (snapshot.Usage.CommittedMemoryPages < snapshot.Limits.CommittedMemoryPages) &&
           (snapshot.Usage.ResidentMemoryPages < snapshot.Limits.ResidentMemoryPages) &&
           (sizeof(Kernel::Memory::MemoryObject) <=
            (snapshot.Limits.KernelMetadataBytes - snapshot.Usage.KernelMetadataBytes));
}

[[nodiscard]] constexpr bool
ResourceUsageHasExactPageIncrease(const Kernel::Resource::ResourceDomainSnapshot& before,
                                  const Kernel::Resource::ResourceDomainSnapshot& after) noexcept
{
    return (after.Identity.GetValue() == before.Identity.GetValue()) && after.Parent.IsEqualTo(before.Parent) &&
           (after.Usage.CommittedMemoryPages == (before.Usage.CommittedMemoryPages + 1U)) &&
           (after.Usage.ResidentMemoryPages == (before.Usage.ResidentMemoryPages + 1U)) &&
           (after.Usage.KernelMetadataBytes ==
            (before.Usage.KernelMetadataBytes + sizeof(Kernel::Memory::MemoryObject))) &&
           (after.Usage.CapabilityHandles == before.Usage.CapabilityHandles) &&
           (after.Usage.IpcQueueBytes == before.Usage.IpcQueueBytes) && (after.Usage.Threads == before.Usage.Threads) &&
           (after.Usage.ChildDomains == before.Usage.ChildDomains) && (after.Depth == before.Depth) &&
           (after.State == before.State) && BytesAreZero(after.Reserved, sizeof(after.Reserved));
}

[[nodiscard]] constexpr bool
ResourceUsageHasExactInitialUserImageIncrease(const Kernel::Resource::ResourceDomainUsage& before,
                                              const Kernel::Resource::ResourceDomainUsage& after) noexcept
{
    return (after.CommittedMemoryPages == before.CommittedMemoryPages) &&
           (after.ResidentMemoryPages == before.ResidentMemoryPages) &&
           (after.KernelMetadataBytes ==
            before.KernelMetadataBytes + InitialUserImageResourceReservation.KernelMetadataBytes) &&
           (after.CapabilityHandles ==
            before.CapabilityHandles + InitialUserImageResourceReservation.CapabilityHandles) &&
           (after.IpcQueueBytes == before.IpcQueueBytes + InitialUserImageResourceReservation.IpcQueueBytes) &&
           (after.Threads == before.Threads) && (after.ChildDomains == before.ChildDomains);
}

[[nodiscard]] constexpr bool ResourceLimitsAreEqual(const Kernel::Resource::ResourceDomainLimits& first,
                                                    const Kernel::Resource::ResourceDomainLimits& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

[[nodiscard]] constexpr bool SharedRootMappingMatches(const Kernel::Memory::AddressSpaceMappingRecord& record,
                                                      const Kernel::Memory::MemoryObjectId memoryObject,
                                                      const Kernel::Memory::FrameId frame,
                                                      const Foundation::UInt64 physicalStart) noexcept
{
    return record.MemoryObject.IsEqualTo(memoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(frame) &&
           (record.VirtualStart.GetValue() == Ia64BspSharedRootShootdownVirtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == 1U) &&
           (record.MemoryObjectOffsetPages == 0U) &&
           (record.Permissions.GetBits() == SupervisorReadWritePermissions().GetBits()) &&
           (record.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (record.Kind == Kernel::Memory::FrameKind::Ram) &&
           (record.Commitment == Kernel::Memory::MemoryCommitment::Pinned) && record.RuntimeMapping &&
           BytesAreZero(record.Reserved, sizeof(record.Reserved));
}

[[nodiscard]] bool MappingContainsWritableRange(Kernel::Memory::AddressSpace& addressSpace,
                                                const Foundation::UInt64 rangeStart,
                                                const Foundation::UInt64 rangeEnd) noexcept
{
    Foundation::UInt64 coveredUntil{rangeStart};
    const Kernel::Memory::AddressSpaceSnapshot snapshot = addressSpace.GetSnapshot();
    for (Foundation::Size scan{0U}; (scan < snapshot.MappingCount) && (coveredUntil < rangeEnd); ++scan)
    {
        const auto mappingResult = addressSpace.TryGetMapping(scan);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& mapping = mappingResult.GetValue();
        const Foundation::UInt64 mappingStart = mapping.VirtualStart.GetValue();
        const Foundation::UInt64 mappingPages = mapping.Pages.GetValue();
        if (mappingPages > (MaximumUInt64 / Kernel::Memory::MappingPageSizeBytes))
        {
            return false;
        }
        Foundation::UInt64 mappingEnd{0U};
        if (!TryGetEnd(mappingStart, mappingPages * Kernel::Memory::MappingPageSizeBytes, mappingEnd))
        {
            return false;
        }
        if ((mappingStart <= coveredUntil) && (coveredUntil < mappingEnd) &&
            mapping.Permissions.Contains(Kernel::Memory::PagePermission::Read) &&
            mapping.Permissions.Contains(Kernel::Memory::PagePermission::Write) &&
            !mapping.Permissions.Contains(Kernel::Memory::PagePermission::User))
        {
            coveredUntil = mappingEnd < rangeEnd ? mappingEnd : rangeEnd;
        }
    }
    return coveredUntil == rangeEnd;
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void FailStopKernelRuntime(const Diagnostics::Ia64BootStage stage,
                                                               const Foundation::UInt64 detail = 0U) noexcept
{
    Diagnostics::StopIa64(Kernel::Diagnostics::StopCode::BootStage, static_cast<Foundation::UInt64>(stage), detail);
}

} // Namespace

Ia64BspKernelRuntime::Ia64BspKernelRuntime(
    Kernel::Object::ObjectRegistry& objectRegistry, Platform::Synchronization::SpinLock& objectRegistrySynchronization,
    const Kernel::Object::CapabilityValidationKey& capabilityValidationKey) noexcept
    : m_objectRegistry{objectRegistry}, m_localTimerProbeTimer{m_localTimer},
      m_revocationRegistry{objectRegistrySynchronization},
      m_capabilitySpace{objectRegistry, m_revocationRegistry, m_localTimer, capabilityValidationKey.Word0,
                        capabilityValidationKey.Word1},
      m_timerQueue{objectRegistry, m_localTimer}, m_deferredTimerWorker{objectRegistry, m_timerQueue},
      m_localTimerProbeDelivery{objectRegistry, m_timerQueue}, m_userProbeWaitSet{objectRegistry},
      m_schedulingContext{objectRegistry}, m_nativePreemptionSchedulingContext{objectRegistry}
{
}

PreparationResult Ia64BspKernelRuntime::FailPreparation(const Ia64BspKernelRuntimeError error) noexcept
{
    m_failure.Error = error;
    m_state = Ia64BspKernelRuntimeState::Failed;
    return PreparationResult::Failure(error);
}

DispatchResult Ia64BspKernelRuntime::FailDispatch(const Ia64BspKernelRuntimeError error) noexcept
{
    m_failure.Error = error;
    m_state = Ia64BspKernelRuntimeState::Failed;
    return DispatchResult::Failure(error);
}

UserProbePreparationResult
Ia64BspKernelRuntime::FailUserProbePreparation(const Ia64BspKernelRuntimeUserProbeError error) noexcept
{
    m_userProbeFailure.Error = error;
    m_userProbeState = Ia64BspKernelRuntimeUserProbeState::PreparationFailed;
    return UserProbePreparationResult::Failure(error);
}

UserProbeDispatchResult
Ia64BspKernelRuntime::FailUserProbeDispatch(const Ia64BspKernelRuntimeUserProbeError error) noexcept
{
    m_userProbeFailure.Error = error;
    m_userProbeState = Ia64BspKernelRuntimeUserProbeState::DispatchFailed;
    m_state = Ia64BspKernelRuntimeState::Failed;
    return UserProbeDispatchResult::Failure(error);
}

InitialUserImagePreparationResult
Ia64BspKernelRuntime::FailInitialUserImagePreparation(const Ia64BspKernelRuntimeInitialUserImageError error,
                                                      const bool terminal) noexcept
{
    m_initialUserImageFailure.Error = error;
    if (terminal)
    {
        m_initialUserImageState = Ia64BspKernelRuntimeInitialUserImageState::PreparationFailed;
    }
    return InitialUserImagePreparationResult::Failure(error);
}

InitialUserImageDispatchResult
Ia64BspKernelRuntime::FailInitialUserImageDispatch(const Ia64BspKernelRuntimeInitialUserImageError error) noexcept
{
    m_initialUserImageFailure.Error = error;
    m_initialUserImageState = Ia64BspKernelRuntimeInitialUserImageState::DispatchFailed;
    m_state = Ia64BspKernelRuntimeState::Failed;
    return InitialUserImageDispatchResult::Failure(error);
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::CaptureInitialUserImageMemoryMapping(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner,
    const Executable::Ia64InitialUserImageMemorySummary& memorySummary,
    const Executable::Ia64InitialUserImageMappingSummary& mappingSummary, const Foundation::Size index,
    Foundation::UInt64& previousEnd, Kernel::Memory::Frame*& aggregateFrame,
    Kernel::Memory::DirectUserMemoryMapping& directMapping) noexcept
{
    const auto rangeResult = memoryOwner.BorrowRange(index);
    const auto mappingResult = mappingOwner.GetMapping(index);
    const auto ledgerResult = m_userRoot.AddressSpaceStorage->TryGetMapping(mappingSummary.InitialMappingCount + index);
    if (!rangeResult.HasValue() || !mappingResult.HasValue() || !ledgerResult.HasValue())
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }

    const Executable::Ia64InitialUserImageMemoryRangeView& range = rangeResult.GetValue();
    const Executable::Ia64InitialUserImageInstalledMapping& installed = mappingResult.GetValue();
    const Executable::Ia64InitialUserImageMemoryRangePlan& plan = range.Plan;
    Foundation::UInt64 rangeEnd{0U};
    if (!InitialUserImagePlansMatch(plan, installed.Plan) ||
        !InitialUserImageReceiptMatches(installed.Receipt, plan, mappingSummary, index) ||
        !InitialUserImageMappingRecordMatches(ledgerResult.GetValue(), plan, memorySummary) ||
        (range.FrameStorage == nullptr) || (range.MemoryObjectStorage == nullptr) || !plan.MemoryObject.IsValid() ||
        !plan.MemoryObject.IsFromRegistry(m_objectRegistry) || (plan.VirtualStart == 0U) ||
        (plan.Pages.GetValue() == 0U) || !plan.Permissions.HasOnlyKnownBits() ||
        !plan.Permissions.Contains(Kernel::Memory::PagePermission::Read) ||
        !plan.Permissions.Contains(Kernel::Memory::PagePermission::User) ||
        (plan.Permissions.Contains(Kernel::Memory::PagePermission::Write) &&
         plan.Permissions.Contains(Kernel::Memory::PagePermission::Execute)) ||
        !TryGetPageRangeEnd(plan.VirtualStart, plan.Pages.GetValue(), rangeEnd) ||
        ((index != 0U) && (plan.VirtualStart < previousEnd)) ||
        !ValidateInitialUserImageMappingObjects(range, memorySummary) ||
        ((aggregateFrame != nullptr) && (aggregateFrame != range.FrameStorage)))
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }

    aggregateFrame = range.FrameStorage;
    directMapping = Kernel::Memory::DirectUserMemoryMapping{
        .UserStart = Kernel::Memory::VirtualAddress{plan.VirtualStart},
        .DirectStart = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(plan.VirtualStart)),
        .Pages = plan.Pages,
        .Permissions = plan.Permissions,
        .Kind = Kernel::Memory::FrameKind::Ram,
    };
    previousEnd = rangeEnd;
    return Ia64BspKernelRuntimeInitialUserImageError::None;
}

bool Ia64BspKernelRuntime::ValidateInitialUserImageMappingObjects(
    const Executable::Ia64InitialUserImageMemoryRangeView& range,
    const Executable::Ia64InitialUserImageMemorySummary& memorySummary) noexcept
{
    const Executable::Ia64InitialUserImageMemoryRangePlan& plan = range.Plan;
    const Kernel::Memory::FrameSnapshot frame = range.FrameStorage->GetSnapshot();
    const Kernel::Memory::MemoryObjectSnapshot memoryObject = range.MemoryObjectStorage->GetSnapshot();
    if ((frame.State != Kernel::Memory::FrameState::Ready) ||
        (frame.Descriptor.PhysicalStart.GetValue() != memorySummary.AggregateArena.PhysicalStart) ||
        (frame.Descriptor.Pages.GetValue() != memorySummary.AggregateArena.PageCount) ||
        (frame.Descriptor.CachePolicy != Kernel::Memory::MemoryCachePolicy::WriteBack) ||
        (frame.Descriptor.Kind != Kernel::Memory::FrameKind::Ram) ||
        !BytesAreZero(frame.Descriptor.Reserved, sizeof(frame.Descriptor.Reserved)) ||
        !BytesAreZero(frame.Reserved, sizeof(frame.Reserved)) ||
        (range.FrameStorage->GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        (memoryObject.State != Kernel::Memory::MemoryObjectState::Ready) ||
        (memoryObject.Descriptor.Pages.GetValue() != plan.Pages.GetValue()) ||
        (memoryObject.Descriptor.CachePolicy != Kernel::Memory::MemoryCachePolicy::WriteBack) ||
        (memoryObject.Descriptor.MaximumPermissions.GetBits() != plan.Permissions.GetBits()) ||
        (memoryObject.Descriptor.Commitment != Kernel::Memory::MemoryCommitment::Pinned) ||
        !BytesAreZero(memoryObject.Descriptor.Reserved, sizeof(memoryObject.Descriptor.Reserved)) ||
        !memoryObject.ResourceDomainIdentity.IsEqualTo(m_resourceDomainIdentity) ||
        (memoryObject.ResourceDomain.GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        (memoryObject.ActiveMappings != 1U) || !BytesAreZero(memoryObject.Reserved, sizeof(memoryObject.Reserved)) ||
        (range.MemoryObjectStorage->GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        return false;
    }

    Kernel::Object::ObjectPin pin;
    m_initialUserImageFailure.UserMemoryPin =
        m_objectRegistry.PinObject(memorySummary.AggregateFrame.GetObjectId(), Kernel::Object::ObjectType::Frame, pin);
    const bool framePinMatches = (m_initialUserImageFailure.UserMemoryPin == Kernel::Object::ObjectPinError::Ok) &&
                                 pin.IsValid() && (&pin.GetObject() == range.FrameStorage);
    pin.Reset();
    if (!framePinMatches)
    {
        return false;
    }

    m_initialUserImageFailure.UserMemoryPin =
        m_objectRegistry.PinObject(plan.MemoryObject.GetObjectId(), Kernel::Object::ObjectType::MemoryObject, pin);
    const bool memoryObjectPinMatches =
        (m_initialUserImageFailure.UserMemoryPin == Kernel::Object::ObjectPinError::Ok) && pin.IsValid() &&
        (&pin.GetObject() == range.MemoryObjectStorage);
    pin.Reset();
    return memoryObjectPinMatches;
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::CaptureInitialUserImageMemoryMappings(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    for (Foundation::Size index{0U}; index < Kernel::Memory::MaximumDirectUserMemoryMappings; ++index)
    {
        m_initialUserImageMemoryMappings[index] = Kernel::Memory::DirectUserMemoryMapping{};
    }
    m_initialUserImageMemoryMappingCount = 0U;

    if ((memoryOwner.GetState() != Executable::Ia64InitialUserImageMemoryOwnerState::Ready) ||
        (mappingOwner.GetState() != Executable::Ia64InitialUserImageMappingOwnerState::Ready) ||
        (m_resourceDomain == nullptr) || !m_resourceDomainIdentity.IsValid() ||
        !m_userRoot.AddressSpaceIdentity.IsValid() || (m_userRoot.AddressSpaceStorage == nullptr) ||
        m_userMemory.IsReady() || m_userMemory.GetAddressSpaceId().IsValid())
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }

    const auto memorySummaryResult = memoryOwner.GetSummary();
    const auto mappingSummaryResult = mappingOwner.GetSummary();
    if (!memorySummaryResult.HasValue() || !mappingSummaryResult.HasValue())
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }
    const Executable::Ia64InitialUserImageMemorySummary& memorySummary = memorySummaryResult.GetValue();
    const Executable::Ia64InitialUserImageMappingSummary& mappingSummary = mappingSummaryResult.GetValue();
    const Foundation::Size rangeCount = memorySummary.RangeCount;
    if ((rangeCount == 0U) || (rangeCount > Kernel::Memory::MaximumDirectUserMemoryMappings) ||
        (mappingSummary.RangeCount != rangeCount) ||
        !mappingSummary.AddressSpaceIdentity.IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        !mappingSummary.AggregateFrame.IsEqualTo(memorySummary.AggregateFrame) ||
        !memorySummary.ResourceDomainGeneration.IsEqualTo(m_resourceDomainIdentity) ||
        (mappingSummary.InitialMappingCount > (MaximumUInt64 - rangeCount)) ||
        (mappingSummary.FinalMappingCount != mappingSummary.InitialMappingCount + rangeCount) ||
        (mappingSummary.FirstMappingSequence == 0U) ||
        (rangeCount - 1U > MaximumUInt64 - mappingSummary.FirstMappingSequence) ||
        (mappingSummary.LastMappingSequence != mappingSummary.FirstMappingSequence + rangeCount - 1U))
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }

    const Kernel::Memory::AddressSpaceSnapshot addressSpace = m_userRoot.AddressSpaceStorage->GetSnapshot();
    if ((addressSpace.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !addressSpace.Identity.IsEqualTo(mappingSummary.AddressSpaceIdentity) ||
        (addressSpace.MappingCount != mappingSummary.FinalMappingCount))
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }

    Foundation::UInt64 previousEnd{0U};
    Kernel::Memory::Frame* aggregateFrame{nullptr};
    for (Foundation::Size index{0U}; index < rangeCount; ++index)
    {
        const auto error =
            CaptureInitialUserImageMemoryMapping(memoryOwner, mappingOwner, memorySummary, mappingSummary, index,
                                                 previousEnd, aggregateFrame, m_initialUserImageMemoryMappings[index]);
        if (error != Ia64BspKernelRuntimeInitialUserImageError::None)
        {
            return error;
        }
    }

    m_initialUserImageMemoryMappingCount = rangeCount;
    return Ia64BspKernelRuntimeInitialUserImageError::None;
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::ReserveInitialUserImageResources() noexcept
{
    if ((m_resourceDomain == nullptr) || !m_resourceDomainIdentity.IsValid() ||
        m_initialUserImageResourceReservationAccepted || !ValidateInitialUserImageResourceDomainRegistration())
    {
        m_initialUserImageFailure.ResourceDomain = Kernel::Resource::ResourceDomainError::InvalidReservation;
        return Ia64BspKernelRuntimeInitialUserImageError::ResourceReservationRejected;
    }

    const Kernel::Resource::ResourceDomainSnapshot before = m_resourceDomain->GetSnapshot();
    if ((before.State != Kernel::Resource::ResourceDomainState::Ready) ||
        (before.Identity.GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        m_initialUserImageFailure.ResourceDomain = Kernel::Resource::ResourceDomainError::NotReady;
        return Ia64BspKernelRuntimeInitialUserImageError::ResourceReservationRejected;
    }

    m_initialUserImageResourceUsageBefore = before.Usage;
    m_initialUserImageFailure.ResourceDomain = m_resourceDomain->TryReserve(InitialUserImageResourceReservation);
    if (m_initialUserImageFailure.ResourceDomain != Kernel::Resource::ResourceDomainError::None)
    {
        return Ia64BspKernelRuntimeInitialUserImageError::ResourceReservationRejected;
    }
    m_initialUserImageResourceReservationAccepted = true;

    const Kernel::Resource::ResourceDomainSnapshot after = m_resourceDomain->GetSnapshot();
    if ((after.State != before.State) || (after.Identity.GetValue() != before.Identity.GetValue()) ||
        !after.Parent.IsEqualTo(before.Parent) || !ResourceLimitsAreEqual(after.Limits, before.Limits) ||
        (after.Depth != before.Depth) || !BytesAreZero(after.Reserved, sizeof(after.Reserved)) ||
        !ResourceUsageHasExactInitialUserImageIncrease(before.Usage, after.Usage))
    {
        return Ia64BspKernelRuntimeInitialUserImageError::PreparationPostconditionRejected;
    }
    return Ia64BspKernelRuntimeInitialUserImageError::None;
}

bool Ia64BspKernelRuntime::ValidateInitialUserImageResourceDomainRegistration() noexcept
{
    if ((m_resourceDomain == nullptr) || !m_resourceDomainIdentity.IsValid() ||
        !m_resourceDomainIdentity.IsFromRegistry(m_objectRegistry))
    {
        return false;
    }
    Kernel::Object::ObjectPin pin;
    m_initialUserImageFailure.ResourceDomainPin = m_objectRegistry.PinObject(
        m_resourceDomainIdentity.GetObjectId(), Kernel::Object::ObjectType::ResourceDomain, pin);
    const bool matches = (m_initialUserImageFailure.ResourceDomainPin == Kernel::Object::ObjectPinError::Ok) &&
                         pin.IsValid() && (&pin.GetObject() == m_resourceDomain);
    pin.Reset();
    return matches;
}

bool Ia64BspKernelRuntime::ValidateInitialUserImageResourceReservation() noexcept
{
    if ((m_resourceDomain == nullptr) || !m_initialUserImageResourceReservationAccepted ||
        !ValidateInitialUserImageResourceDomainRegistration())
    {
        return false;
    }
    const Kernel::Resource::ResourceDomainSnapshot snapshot = m_resourceDomain->GetSnapshot();
    return (snapshot.State == Kernel::Resource::ResourceDomainState::Ready) &&
           (snapshot.Identity.GetValue() == m_resourceDomain->GetIdentifier().GetValue()) &&
           ResourceUsageHasExactInitialUserImageIncrease(m_initialUserImageResourceUsageBefore, snapshot.Usage);
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::InitializeInitialUserImageEndpoint() noexcept
{
    if ((m_issuer == nullptr) || (m_resourceDomain == nullptr) || !m_issuer->IsFromRegistry(m_objectRegistry) ||
        (m_capabilitySpace.GetActiveCount() != 0U) || (m_initialUserImageEndpoint != nullptr) ||
        m_initialUserImageEndpointIdentity.IsValid() || (m_initialUserImageMemoryMappingCount == 0U))
    {
        return Ia64BspKernelRuntimeInitialUserImageError::EndpointInitializationRejected;
    }

    m_initialUserImageEndpoint = ::new (static_cast<void*>(m_initialUserImageEndpointStorage)) Kernel::Ipc::Endpoint{
        m_capabilitySpace, m_resourceDomain->GetIdentifier(), InitialUserImageEndpointConfiguration};
    const Kernel::Ipc::EndpointQueueState initialQueue = m_initialUserImageEndpoint->GetQueueState();
    if (!m_initialUserImageEndpoint->IsReady() ||
        !m_initialUserImageEndpoint->HasReceiverCapabilitySpace(m_capabilitySpace) ||
        (m_initialUserImageEndpoint->GetObjectType() != Kernel::Object::ObjectType::Endpoint) ||
        (m_initialUserImageEndpoint->GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        (initialQueue.QueuedBytes != 0U) || (initialQueue.NextSequence != 1U) || (initialQueue.QueuedMessages != 0U) ||
        (initialQueue.QueuedAttachments != 0U) || !BytesAreZero(initialQueue.Reserved, sizeof(initialQueue.Reserved)))
    {
        return Ia64BspKernelRuntimeInitialUserImageError::EndpointInitializationRejected;
    }

    const auto registered = m_issuer->CreateObject(*m_initialUserImageEndpoint);
    if (!registered.HasValue())
    {
        m_initialUserImageFailure.EndpointRegistry = registered.GetError();
        return Ia64BspKernelRuntimeInitialUserImageError::EndpointRegistrationRejected;
    }
    m_initialUserImageEndpointIdentity = registered.GetValue();

    Kernel::Object::ObjectPin endpointPin;
    m_initialUserImageFailure.EndpointPin = m_objectRegistry.PinObject(
        m_initialUserImageEndpointIdentity, Kernel::Object::ObjectType::Endpoint, endpointPin);
    const bool registrationMatches =
        (m_initialUserImageFailure.EndpointPin == Kernel::Object::ObjectPinError::Ok) && endpointPin.IsValid() &&
        (endpointPin.GetObjectId().GetValue() == m_initialUserImageEndpointIdentity.GetValue()) &&
        (endpointPin.GetObjectType() == Kernel::Object::ObjectType::Endpoint) &&
        (&endpointPin.GetObject() == m_initialUserImageEndpoint) &&
        (endpointPin.GetObject().GetChargeOwner().GetValue() == m_resourceDomain->GetIdentifier().GetValue());
    endpointPin.Reset();
    return registrationMatches ? Ia64BspKernelRuntimeInitialUserImageError::None
                               : Ia64BspKernelRuntimeInitialUserImageError::EndpointRegistrationRejected;
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::InitializeInitialUserImageDiagnosticChannel() noexcept
{
    if ((m_issuer == nullptr) || (m_resourceDomain == nullptr) || (m_initialUserImageDiagnosticChannel != nullptr) ||
        m_initialUserImageDiagnosticChannelIdentity.IsValid())
    {
        return Ia64BspKernelRuntimeInitialUserImageError::DiagnosticChannelInitializationRejected;
    }

    // Lines written through the channel reach the platform debug channel as
    // USER: lines; the channel is charged like the boot Endpoint.
    m_initialUserImageDiagnosticChannel = ::new (static_cast<void*>(m_initialUserImageDiagnosticChannelStorage))
        Kernel::Diagnostics::DiagnosticChannel{m_initialUserImageDiagnosticSink, m_resourceDomain->GetIdentifier()};
    const auto registered = m_issuer->CreateObject(*m_initialUserImageDiagnosticChannel);
    if (!registered.HasValue())
    {
        m_initialUserImageFailure.DiagnosticChannelRegistry = registered.GetError();
        return Ia64BspKernelRuntimeInitialUserImageError::DiagnosticChannelRegistrationRejected;
    }
    m_initialUserImageDiagnosticChannelIdentity = registered.GetValue();
    return IsInitialUserImageDiagnosticChannelRegistered()
               ? Ia64BspKernelRuntimeInitialUserImageError::None
               : Ia64BspKernelRuntimeInitialUserImageError::DiagnosticChannelRegistrationRejected;
}

bool Ia64BspKernelRuntime::IsInitialUserImageDiagnosticChannelRegistered() noexcept
{
    if ((m_resourceDomain == nullptr) || (m_initialUserImageDiagnosticChannel == nullptr) ||
        !m_initialUserImageDiagnosticChannelIdentity.IsValid())
    {
        return false;
    }
    Kernel::Object::ObjectPin channelPin;
    m_initialUserImageFailure.DiagnosticChannelPin = m_objectRegistry.PinObject(
        m_initialUserImageDiagnosticChannelIdentity, Kernel::Object::ObjectType::DiagnosticChannel, channelPin);
    const bool registrationMatches =
        (m_initialUserImageFailure.DiagnosticChannelPin == Kernel::Object::ObjectPinError::Ok) &&
        channelPin.IsValid() && (&channelPin.GetObject() == m_initialUserImageDiagnosticChannel) &&
        (channelPin.GetObject().GetChargeOwner().GetValue() == m_resourceDomain->GetIdentifier().GetValue());
    channelPin.Reset();
    return registrationMatches;
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::InitializeInitialUserImageMemory() noexcept
{
    if ((m_initialUserImageEndpoint == nullptr) || !m_initialUserImageEndpointIdentity.IsValid() ||
        (m_initialUserImageMemoryMappingCount == 0U) ||
        (m_initialUserImageMemoryMappingCount > Kernel::Memory::MaximumDirectUserMemoryMappings) ||
        m_userMemory.IsReady() || m_userMemory.GetAddressSpaceId().IsValid())
    {
        m_initialUserImageFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }
    m_initialUserImageFailure.UserMemory =
        m_userMemory.Initialize(m_userRoot.AddressSpaceIdentity, m_initialUserImageMemoryMappings,
                                m_initialUserImageMemoryMappingCount, m_userRoot.AddressSpaceStorage);
    if (m_initialUserImageFailure.UserMemory != Kernel::Memory::DirectUserMemoryAccessError::None)
    {
        return Ia64BspKernelRuntimeInitialUserImageError::UserMemoryInitializationRejected;
    }
    return Ia64BspKernelRuntimeInitialUserImageError::None;
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::PrepareInitialUserImageLaunchOwner(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    const Ia64UserModeVirtualRange supervisorMemoryStack{
        .VirtualStart = AddressOf(m_workerMemoryStack),
        .SizeBytes = sizeof(m_workerMemoryStack),
    };
    const Ia64UserModeVirtualRange supervisorRseBackingStore{
        .VirtualStart = AddressOf(m_workerRseBackingStore),
        .SizeBytes = sizeof(m_workerRseBackingStore),
    };
    const auto prepared = m_initialUserImageLaunchOwner.Prepare(
        memoryOwner, mappingOwner, m_userRoot.AddressSpaceIdentity, m_objectRegistry, *m_issuer, m_revocationRegistry,
        m_capabilitySpace,
        Ia64InitialUserImageBootstrapGrant{
            .Registry = &m_objectRegistry,
            .Target = m_initialUserImageEndpointIdentity,
            .ExpectedType = Kernel::Object::ObjectType::Endpoint,
            .Rights = InitialUserImageEndpointRights,
            .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
            .SealedObjectSelector = 0U,
        },
        Ia64InitialUserImageBootstrapGrant{
            .Registry = &m_objectRegistry,
            .Target = m_initialUserImageDiagnosticChannelIdentity,
            .ExpectedType = Kernel::Object::ObjectType::DiagnosticChannel,
            .Rights = InitialUserImageDiagnosticChannelRights,
            .LeaseDeadline = Kernel::Object::MonotonicTime::Infinite(),
            .SealedObjectSelector = 0U,
        },
        m_protectionDomainIdentity, m_protectionDomain, m_resourceDomainIdentity, *m_resourceDomain,
        supervisorMemoryStack, supervisorRseBackingStore,
        m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit);
    if (!prepared.HasValue())
    {
        m_initialUserImageFailure.LaunchOwner = prepared.GetErrorReference();
        return Ia64BspKernelRuntimeInitialUserImageError::LaunchPreparationRejected;
    }
    const Ia64InitialUserImageLaunchSummary& summary = prepared.GetValue();
    if ((m_initialUserImageLaunchOwner.GetState() != Ia64InitialUserImageLaunchOwnerState::Ready) ||
        !summary.AddressSpaceIdentity.IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        !summary.ProtectionDomainIdentity.IsEqualTo(m_protectionDomainIdentity))
    {
        return Ia64BspKernelRuntimeInitialUserImageError::LaunchPreparationRejected;
    }
    return Ia64BspKernelRuntimeInitialUserImageError::None;
}

Ia64BspKernelRuntimeInitialUserImageError Ia64BspKernelRuntime::RetainInitialUserImageLaunchRequest() noexcept
{
    {
        const auto request = m_initialUserImageLaunchOwner.GetLaunchRequest();
        if (!request.HasValue())
        {
            return Ia64BspKernelRuntimeInitialUserImageError::LaunchPreparationRejected;
        }
        m_initialUserImageLaunchRequest = request.GetValue();
    }
    {
        const auto summary = m_initialUserImageLaunchOwner.GetSummary();
        if (!summary.HasValue() ||
            (m_initialUserImageLaunchRequest.InitialArguments.Register4 !=
             summary.GetValue().BootstrapCapability.GetSelector()) ||
            (m_initialUserImageLaunchRequest.InitialArguments.Register5 !=
             summary.GetValue().BootstrapCapability.GetAuthenticator()) ||
            (m_initialUserImageLaunchRequest.InitialArguments.Register6 !=
             summary.GetValue().DiagnosticCapability.GetSelector()) ||
            (m_initialUserImageLaunchRequest.InitialArguments.Register7 !=
             summary.GetValue().DiagnosticCapability.GetAuthenticator()))
        {
            return Ia64BspKernelRuntimeInitialUserImageError::LaunchPreparationRejected;
        }
    }
    if (!Ia64UserModeLaunch::Validate(m_initialUserImageLaunchRequest).HasValue())
    {
        return Ia64BspKernelRuntimeInitialUserImageError::LaunchPreparationRejected;
    }
    return Ia64BspKernelRuntimeInitialUserImageError::None;
}

bool Ia64BspKernelRuntime::ValidateInitialUserImageIpcPostconditions() noexcept
{
    if ((m_runtimeMemory == nullptr) || (m_resourceDomain == nullptr) || (m_runLoop == nullptr) ||
        (m_initialUserImageEndpoint == nullptr) || !m_initialUserImageEndpointIdentity.IsValid() ||
        (m_initialUserImageMemoryMappingCount == 0U) ||
        (m_initialUserImageMemoryMappingCount > Kernel::Memory::MaximumDirectUserMemoryMappings) ||
        !ValidateInitialUserImageResourceReservation() || !m_userMemory.IsReady() ||
        !m_userMemory.GetAddressSpaceId().IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        (m_systemCallBridgeContext.RunLoop != m_runLoop) || (m_systemCallBridgeContext.UserMemory != &m_userMemory) ||
        (m_systemCallBridgeContext.Scratch != &m_systemCallScratch) ||
        (m_systemCallBridgeContext.WaitContinuation != nullptr) ||
        (m_systemCallBridgeContext.WaitSetDispatcher != nullptr) || !m_initialUserImageEndpoint->IsReady() ||
        !m_initialUserImageEndpoint->HasReceiverCapabilitySpace(m_capabilitySpace) ||
        (m_initialUserImageEndpoint->GetObjectType() != Kernel::Object::ObjectType::Endpoint) ||
        (m_initialUserImageEndpoint->GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        (m_capabilitySpace.GetActiveCount() != 2U))
    {
        return false;
    }

    const Kernel::Ipc::EndpointQueueState queue = m_initialUserImageEndpoint->GetQueueState();
    if ((queue.QueuedBytes != 0U) || (queue.NextSequence != 1U) || (queue.QueuedMessages != 0U) ||
        (queue.QueuedAttachments != 0U) || !BytesAreZero(queue.Reserved, sizeof(queue.Reserved)))
    {
        return false;
    }

    Kernel::Object::ObjectPin endpointPin;
    m_initialUserImageFailure.EndpointPin = m_objectRegistry.PinObject(
        m_initialUserImageEndpointIdentity, Kernel::Object::ObjectType::Endpoint, endpointPin);
    const bool registrationMatches =
        (m_initialUserImageFailure.EndpointPin == Kernel::Object::ObjectPinError::Ok) && endpointPin.IsValid() &&
        (endpointPin.GetObjectId().GetValue() == m_initialUserImageEndpointIdentity.GetValue()) &&
        (&endpointPin.GetObject() == m_initialUserImageEndpoint) &&
        (endpointPin.GetObject().GetChargeOwner().GetValue() == m_resourceDomain->GetIdentifier().GetValue());
    endpointPin.Reset();
    if (!registrationMatches)
    {
        return false;
    }

    const auto summary = m_initialUserImageLaunchOwner.GetSummary();
    if (!summary.HasValue())
    {
        return false;
    }
    const Kernel::Object::CapabilityHandle capability = summary.GetValue().BootstrapCapability;
    const auto descriptor = m_capabilitySpace.Query(capability);
    if (!descriptor.HasValue() || !capability.IsValid() ||
        (descriptor.GetValue().Object.GetValue() != m_initialUserImageEndpointIdentity.GetValue()) ||
        (descriptor.GetValue().Type != Kernel::Object::ObjectType::Endpoint) ||
        !descriptor.GetValue().Rights.IsEqualTo(InitialUserImageEndpointRights) ||
        !descriptor.GetValue().RevocationDomain.IsValid() || !descriptor.GetValue().LeaseDeadline.IsInfinite() ||
        (descriptor.GetValue().SealedObjectSelector != 0U) ||
        (descriptor.GetValue().ChargeOwner.GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        return false;
    }

    const Memory::Ia64BspRuntimeSystemCallPublicationSnapshot publication =
        m_runtimeMemory->GetSystemCallPublicationSnapshot();
    return publication.Published && (publication.LastError == Memory::Ia64BspRuntimeSystemCallPublicationError::None) &&
           (publication.Context.BridgeContextAddress == AddressOf(&m_systemCallBridgeContext)) &&
           (publication.Context.ProcessorIndex == m_root.Processor.GetValue());
}

bool Ia64BspKernelRuntime::ValidateInitialUserImageDiagnosticChannelPostconditions() noexcept
{
    if (!IsInitialUserImageDiagnosticChannelRegistered())
    {
        return false;
    }
    const auto summary = m_initialUserImageLaunchOwner.GetSummary();
    if (!summary.HasValue())
    {
        return false;
    }

    // The r6/r7 capability names the channel with exactly Inspect|Write and
    // an infinite lease, in a revocation root of its own.
    const Kernel::Object::CapabilityHandle capability = summary.GetValue().DiagnosticCapability;
    const auto descriptor = m_capabilitySpace.Query(capability);
    return descriptor.HasValue() && capability.IsValid() &&
           (descriptor.GetValue().Object.GetValue() == m_initialUserImageDiagnosticChannelIdentity.GetValue()) &&
           (descriptor.GetValue().Type == Kernel::Object::ObjectType::DiagnosticChannel) &&
           descriptor.GetValue().Rights.IsEqualTo(InitialUserImageDiagnosticChannelRights) &&
           descriptor.GetValue().RevocationDomain.IsValid() &&
           (descriptor.GetValue().RevocationDomain.GetValue() != summary.GetValue().RevocationDomain.GetValue()) &&
           descriptor.GetValue().LeaseDeadline.IsInfinite() && (descriptor.GetValue().SealedObjectSelector == 0U) &&
           (descriptor.GetValue().ChargeOwner.GetValue() == m_resourceDomain->GetIdentifier().GetValue());
}

LocalTimerProbeResult
Ia64BspKernelRuntime::FailLocalTimerProbe(const Ia64BspKernelRuntimeLocalTimerProbeError error) noexcept
{
    m_localTimerProbeFailure.Error = error;
    m_localTimerProbeState = Ia64BspKernelRuntimeLocalTimerProbeState::Failed;
    return LocalTimerProbeResult::Failure(error);
}

Ia64BspKernelRuntime::SharedRootShootdownResult
Ia64BspKernelRuntime::FailSharedRootShootdown(const Ia64BspSharedRootShootdownError error) noexcept
{
    m_sharedRootShootdownFailure.Error = error;
    m_sharedRootShootdownState = Ia64BspSharedRootShootdownState::Failed;
    m_sharedRootShootdownSummary.State = Ia64BspSharedRootShootdownState::Failed;
    return SharedRootShootdownResult::Failure(error);
}

Ia64BspKernelRuntime::SharedRootShootdownResult
Ia64BspKernelRuntime::FailPendingSharedRootShootdown(const Ia64BspSharedRootShootdownError error) noexcept
{
    m_sharedRootShootdownFailure.Settlement =
        m_root.AddressSpaceStorage->FailShootdown(m_sharedRootShootdownRequest.GetToken());
    if (m_sharedRootShootdownFailure.Settlement != Kernel::Memory::AddressSpaceError::None)
    {
        return FailSharedRootShootdown(Ia64BspSharedRootShootdownError::ShootdownSettlementRejected);
    }
    return FailSharedRootShootdown(error);
}

bool Ia64BspKernelRuntime::ValidateExternalInterruptPublication(
    const Memory::Ia64BspRuntimeExternalInterruptPublicationSnapshot& publication) const noexcept
{
    Foundation::UInt64 contextEnd{0U};
    Foundation::UInt64 evidenceEnd{0U};
    if (!publication.Published ||
        (publication.LastError != Memory::Ia64BspRuntimeExternalInterruptPublicationError::None) ||
        (publication.LastInterruptVectorTableError != Memory::Ia64RuntimeExternalInterruptPublicationError::None) ||
        (publication.ContextAddress == 0U) || (publication.EvidenceAddress == 0U) ||
        ((publication.ContextAddress & (alignof(Interrupt::Ia64ExternalInterruptEntryContext) - 1U)) != 0U) ||
        ((publication.EvidenceAddress & (alignof(Interrupt::Ia64LocalTimerInterruptEvidence) - 1U)) != 0U) ||
        !TryGetEnd(publication.ContextAddress, sizeof(publication.Context), contextEnd) ||
        !TryGetEnd(publication.EvidenceAddress, sizeof(publication.Evidence), evidenceEnd) ||
        (contextEnd > RegionZeroLimit) || (evidenceEnd > RegionZeroLimit) ||
        ((publication.ContextAddress < evidenceEnd) && (publication.EvidenceAddress < contextEnd)) ||
        (publication.Context.EvidenceAddress != publication.EvidenceAddress) ||
        (publication.Context.ProcessorIndex != m_root.Processor.GetValue()) ||
        (publication.Context.TimerVector != Interrupt::Ia64ExternalInterruptTimerVector) ||
        (publication.Context.TimerVector != m_timerVector) ||
        (publication.Context.ExternalPreemptionControlAddress != 0U) || (publication.Evidence.Sequence != 0U) ||
        (publication.Evidence.ProgrammedDeadlineTicks != 0U) ||
        (publication.Evidence.ObservedIntervalTimeCounter != 0U) ||
        (publication.Evidence.InterruptedInstructionPointer != 0U) ||
        (publication.Evidence.InterruptedProcessorStatus != 0U) || (publication.Evidence.InterruptedIfsValid != 0U) ||
        (publication.Evidence.ProcessorIndex != 0U) || (publication.Evidence.Vector != 0U))
    {
        return false;
    }
    for (const Foundation::UInt8 value : publication.Evidence.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    for (const Foundation::UInt8 value : publication.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

void Ia64BspKernelRuntime::RecordLocalTimerProbeEvidence() const noexcept
{
    Platform::Ia64::Diagnostics::Ia64UserProbeEvidence& evidence =
        Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().UserProbe;
    evidence.LocalTimerInterrupt = m_localTimerProbeEvidence;
    evidence.TimerQueue = m_localTimerProbeTimerQueueEvidence;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.LocalTimerPublished);
}

bool Ia64BspKernelRuntime::ValidateLocalTimerProbeCommitPostconditions() const noexcept
{
    if (m_controller == nullptr)
    {
        return false;
    }
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    const Kernel::Time::TimerSnapshot probeTimer = m_localTimerProbeTimer.GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot notification = m_localTimerProbeNotification.GetSnapshot();
    const Kernel::Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Foundation::UInt8 timerQueueSource =
        static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue);
    return (m_localTimerProbeEvidence.Sequence == 1U) && (m_localTimerProbeEvidence.AcknowledgedSequence == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.Sequence == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.TimerObjectIdentity == m_localTimerProbeTimerIdentity.GetValue()) &&
           (m_localTimerProbeTimerQueueEvidence.NotificationObjectIdentity ==
            m_localTimerProbeNotificationIdentity.GetValue()) &&
           (m_localTimerProbeTimerQueueEvidence.TimerArmSequence == 1U) &&
           (m_localTimerProbeTimerQueueEvidence.DeadlineTicks == m_localTimerProbeEvidence.ProgrammedDeadlineTicks) &&
           (m_localTimerProbeTimerQueueEvidence.ObservedTicks ==
            m_localTimerProbeDeliveryReceipt.Expiration.ObservedTicks) &&
           (m_localTimerProbeTimerQueueEvidence.ObservedTicks >=
            m_localTimerProbeEvidence.ObservedIntervalTimeCounter) &&
           (m_localTimerProbeTimerQueueEvidence.QueueClaimSequence == 1U) &&
           (m_localTimerProbeTimerQueueEvidence.NotificationClaimSequence == 1U) &&
           (m_localTimerProbeTimerQueueEvidence.InitialDeadlineState ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineState::Armed)) &&
           (m_localTimerProbeTimerQueueEvidence.SelectedDeadlineSources == timerQueueSource) &&
           (m_localTimerProbeTimerQueueEvidence.InitialDeadlineReprogrammed == 1U) &&
           (m_localTimerProbeTimerQueueEvidence.ClaimedTimerState ==
            static_cast<Foundation::UInt8>(Kernel::Time::TimerState::Expired)) &&
           (m_localTimerProbeTimerQueueEvidence.NotificationPendingAfterSignal == 1U) &&
           (m_localTimerProbeTimerQueueEvidence.FinalTimerState ==
            static_cast<Foundation::UInt8>(Kernel::Time::TimerState::Disarmed)) &&
           (m_localTimerProbeTimerQueueEvidence.FinalNotificationState ==
            static_cast<Foundation::UInt8>(Kernel::Ipc::NotificationState::Ready)) &&
           (m_localTimerProbeTimerQueueEvidence.FinalNotificationPending == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.FinalDeadlineState ==
            static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineState::Disarmed)) &&
           (m_localTimerProbeTimerQueueEvidence.FinalLocalTimerState ==
            static_cast<Foundation::UInt8>(Time::Ia64LocalTimerState::Ready)) &&
           (m_localTimerProbeTimerQueueEvidence.FinalLocalTimerArmed == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.FinalScheduledEntries == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.FinalPendingEntries == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.FinalClaimedEntries == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.Reserved[0U] == 0U) &&
           (m_localTimerProbeTimerQueueEvidence.Reserved[1U] == 0U) &&
           (m_state == Ia64BspKernelRuntimeState::Prepared) &&
           (m_userProbeState == Ia64BspKernelRuntimeUserProbeState::Prepared) &&
           (timer.State == Time::Ia64LocalTimerState::Ready) &&
           (timer.Vector == Interrupt::Ia64ExternalInterruptTimerVector) && (timer.Vector == m_timerVector) &&
           (timer.Armed == 0U) && (timer.ProgrammedDeadlineTicks == 0U) &&
           (probeTimer.State == Kernel::Time::TimerState::Disarmed) && !probeTimer.Token.IsValid() &&
           (probeTimer.DeadlineTicks == 0U) && (notification.State == Kernel::Ipc::NotificationState::Ready) &&
           (notification.Pending == 0U) && (notification.LastClaimSequence == 1U) &&
           (timerQueue.NextDeadlineTicks == 0U) && (timerQueue.NextClaimSequence == 2U) &&
           (timerQueue.ScheduledEntries == 0U) && (timerQueue.PendingEntries == 0U) &&
           (timerQueue.ClaimedEntries == 0U) && (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (controller.Deadline.SelectedDeadlineTicks == 0U) && (controller.Deadline.SelectedSources == 0U) &&
           ValidateLocalTimerProbeObjects();
}

bool Ia64BspKernelRuntime::ValidateLocalTimerProbeObjects() const noexcept
{
    if ((m_resourceDomain == nullptr) || !m_localTimerProbeTimerIdentity.IsValid() ||
        !m_localTimerProbeTimerIdentity.IsFromRegistry(m_objectRegistry) ||
        !m_localTimerProbeNotificationIdentity.IsValid() ||
        !m_localTimerProbeNotificationIdentity.IsFromRegistry(m_objectRegistry) ||
        (m_localTimerProbeTimerIdentity.GetValue() == m_localTimerProbeNotificationIdentity.GetValue()) ||
        (m_localTimerProbeTimer.GetObjectType() != Kernel::Object::ObjectType::Timer) ||
        (m_localTimerProbeNotification.GetObjectType() != Kernel::Object::ObjectType::Notification) ||
        (m_localTimerProbeTimer.GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        (m_localTimerProbeNotification.GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        return false;
    }

    Kernel::Object::ObjectPin timerPin;
    Kernel::Object::ObjectPin notificationPin;
    const Kernel::Object::ObjectPinError timerPinError = m_objectRegistry.PinObject(
        m_localTimerProbeTimerIdentity.GetObjectId(), Kernel::Object::ObjectType::Timer, timerPin);
    const Kernel::Object::ObjectPinError notificationPinError = m_objectRegistry.PinObject(
        m_localTimerProbeNotificationIdentity.GetObjectId(), Kernel::Object::ObjectType::Notification, notificationPin);
    const bool matches = (timerPinError == Kernel::Object::ObjectPinError::Ok) &&
                         (notificationPinError == Kernel::Object::ObjectPinError::Ok) && timerPin.IsValid() &&
                         notificationPin.IsValid() && (&timerPin.GetObject() == &m_localTimerProbeTimer) &&
                         (&notificationPin.GetObject() == &m_localTimerProbeNotification);
    notificationPin.Reset();
    timerPin.Reset();
    return matches;
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::PrepareLocalTimerProbePublication() noexcept
{
    m_localTimerProbeFailure.Publication = m_runtimeMemory->PublishExternalInterruptEntryContext(m_root.Processor);
    if (m_localTimerProbeFailure.Publication != Memory::Ia64BspRuntimeExternalInterruptPublicationError::None)
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::ExternalInterruptPublicationRejected;
        return false;
    }

    m_localTimerProbePublication = m_runtimeMemory->GetExternalInterruptPublicationSnapshot();
    m_localTimerProbeFailure.InterruptVectorTable = m_localTimerProbePublication.LastInterruptVectorTableError;
    if (!ValidateExternalInterruptPublication(m_localTimerProbePublication))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::ExternalInterruptPublicationMismatch;
        return false;
    }
    m_localTimerProbePublishedContextAddress = m_localTimerProbePublication.ContextAddress;
    m_localTimerProbePublishedEvidenceAddress = m_localTimerProbePublication.EvidenceAddress;
    return true;
}

ASTERIA_NOINLINE_NOIPA bool
Ia64BspKernelRuntime::RefreshLocalTimerProbeInitialDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    const auto refreshed = Controller().RefreshTimerQueueDeadline(m_localTimerProbeArmReceipt.Timer.ObservedTicks);
    if (!refreshed.HasValue())
    {
        m_localTimerProbeFailure.Controller = refreshed.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRefreshRejected;
        return false;
    }
    m_localTimerProbeInitialDeadlineReceipt = refreshed.GetValue();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Time::Ia64LocalTimerSnapshot localTimer = m_localTimer.GetSnapshot();
    const Foundation::UInt8 timerQueueSource =
        static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue);
    if ((m_localTimerProbeInitialDeadlineReceipt.SelectedDeadlineTicks != deadlineTicks) ||
        (m_localTimerProbeInitialDeadlineReceipt.ObservedTicks < m_localTimerProbeArmReceipt.Timer.ObservedTicks) ||
        (m_localTimerProbeInitialDeadlineReceipt.ObservedTicks >= deadlineTicks) ||
        (m_localTimerProbeInitialDeadlineReceipt.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
        (m_localTimerProbeInitialDeadlineReceipt.SelectedSources != timerQueueSource) ||
        (m_localTimerProbeInitialDeadlineReceipt.Reprogrammed != 1U) ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Idle) ||
        (controller.Deadline.SelectedDeadlineTicks != deadlineTicks) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
        (controller.Deadline.SelectedSources != timerQueueSource) ||
        (localTimer.State != Time::Ia64LocalTimerState::Ready) || (localTimer.Vector != m_timerVector) ||
        (localTimer.ProgrammedDeadlineTicks != deadlineTicks) || (localTimer.Armed != 1U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRefreshRejected;
        return false;
    }
    return true;
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::PrepareLocalTimerProbeAttempt() noexcept
{
    if (!PrepareLocalTimerProbePublication())
    {
        return false;
    }

    const Foundation::UInt64 initialTicks = m_localTimer.GetTicks();
    m_localTimerProbeFailure.InitialTicks = initialTicks;
    if (initialTicks == MaximumUInt64)
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::ClockExhausted;
        return false;
    }
    if (initialTicks >= (MaximumUInt64 - Ia64BspLocalTimerProbeLeadTicks))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineOverflow;
        return false;
    }
    const Foundation::UInt64 deadlineTicks = initialTicks + Ia64BspLocalTimerProbeLeadTicks;
    m_localTimerProbeFailure.ProgrammedDeadlineTicks = deadlineTicks;
    if (deadlineTicks >= (MaximumUInt64 - Ia64BspLocalTimerProbeGraceTicks))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineOverflow;
        return false;
    }
    m_localTimerProbeFailure.AbsoluteTimeoutTicks = deadlineTicks + Ia64BspLocalTimerProbeGraceTicks;

    const auto armed = m_timerQueue.Arm(m_localTimerProbeTimerIdentity, m_localTimerProbeTimer,
                                        Kernel::Object::MonotonicTime{deadlineTicks});
    if (!armed.HasValue())
    {
        m_localTimerProbeFailure.TimerQueue = armed.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerArmRejected;
        return false;
    }

    m_localTimerProbeArmReceipt = armed.GetValue();
    const Kernel::Time::TimerQueueState armedQueue = m_timerQueue.GetState();
    const Kernel::Time::TimerSnapshot armedTimer = m_localTimerProbeTimer.GetSnapshot();
    if (!m_localTimerProbeArmReceipt.Timer.Token.IsValid() ||
        (m_localTimerProbeArmReceipt.Timer.Token.GetSequence() != 1U) || m_localTimerProbeArmReceipt.Timer.Replaced ||
        (m_localTimerProbeArmReceipt.Timer.DeadlineTicks != deadlineTicks) ||
        (m_localTimerProbeArmReceipt.Timer.ObservedTicks < initialTicks) ||
        (m_localTimerProbeArmReceipt.Timer.ObservedTicks >= deadlineTicks) ||
        (m_localTimerProbeArmReceipt.NextDeadlineTicks != deadlineTicks) ||
        !armedTimer.Token.IsEqualTo(m_localTimerProbeArmReceipt.Timer.Token) ||
        (armedTimer.DeadlineTicks != deadlineTicks) || (armedTimer.State != Kernel::Time::TimerState::Armed) ||
        (armedQueue.NextDeadlineTicks != deadlineTicks) || (armedQueue.NextClaimSequence != 1U) ||
        (armedQueue.ScheduledEntries != 1U) || (armedQueue.PendingEntries != 0U) || (armedQueue.ClaimedEntries != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerArmRejected;
        return false;
    }

    return RefreshLocalTimerProbeInitialDeadline(deadlineTicks);
}

bool Ia64BspKernelRuntime::HasPossiblyAcceptedLocalTimerProbeArm() const noexcept
{
    const Kernel::Time::TimerQueueState queue = m_timerQueue.GetState();
    const Kernel::Time::TimerSnapshot timer = m_localTimerProbeTimer.GetSnapshot();
    return m_localTimerProbeArmReceipt.Timer.Token.IsValid() || (queue.NextDeadlineTicks != 0U) ||
           (queue.NextClaimSequence != 1U) || (queue.ScheduledEntries != 0U) || (queue.PendingEntries != 0U) ||
           (queue.ClaimedEntries != 0U) || (timer.State != Kernel::Time::TimerState::Disarmed) ||
           timer.Token.IsValid() || (timer.DeadlineTicks != 0U);
}

bool Ia64BspKernelRuntime::RollbackLocalTimerProbeBeforeCompletion() noexcept
{
    const Kernel::Time::TimerQueueState before = m_timerQueue.GetState();
    if ((before.PendingEntries != 0U) || (before.ClaimedEntries != 0U) || (before.NextClaimSequence != 1U) ||
        (before.ScheduledEntries > 1U) || ((before.ScheduledEntries == 0U) && (before.NextDeadlineTicks != 0U)) ||
        ((before.ScheduledEntries == 1U) &&
         ((before.NextDeadlineTicks != m_localTimerProbeFailure.ProgrammedDeadlineTicks) ||
          !m_localTimerProbeArmReceipt.Timer.Token.IsValid())))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerRollbackRejected;
        return false;
    }

    if (before.ScheduledEntries == 1U)
    {
        const auto cancelled = m_timerQueue.Cancel(m_localTimerProbeTimerIdentity, m_localTimerProbeTimer,
                                                   m_localTimerProbeArmReceipt.Timer.Token);
        if (!cancelled.HasValue())
        {
            m_localTimerProbeFailure.TimerQueue = cancelled.GetError();
            m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerRollbackRejected;
            return false;
        }
        const Kernel::Time::TimerQueueCancelReceipt& receipt = cancelled.GetValue();
        if ((receipt.Timer.State != Kernel::Time::TimerState::Disarmed) || receipt.Timer.Token.IsValid() ||
            (receipt.Timer.DeadlineTicks != 0U) || (receipt.NextDeadlineTicks != 0U))
        {
            m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerRollbackRejected;
            return false;
        }
    }

    const Kernel::Time::TimerQueueState emptyQueue = m_timerQueue.GetState();
    const Kernel::Time::TimerSnapshot timer = m_localTimerProbeTimer.GetSnapshot();
    if ((emptyQueue.NextDeadlineTicks != 0U) || (emptyQueue.NextClaimSequence != 1U) ||
        (emptyQueue.ScheduledEntries != 0U) || (emptyQueue.PendingEntries != 0U) || (emptyQueue.ClaimedEntries != 0U) ||
        (timer.State != Kernel::Time::TimerState::Disarmed) || timer.Token.IsValid() || (timer.DeadlineTicks != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerRollbackRejected;
        return false;
    }

    return RefreshLocalTimerProbeRollbackDeadline();
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::RefreshLocalTimerProbeRollbackDeadline() noexcept
{
    const Foundation::UInt64 minimumObservedTicks = m_localTimerProbeInitialDeadlineReceipt.ObservedTicks != 0U
                                                        ? m_localTimerProbeInitialDeadlineReceipt.ObservedTicks
                                                        : m_localTimerProbeArmReceipt.Timer.ObservedTicks;
    const auto refreshed = Controller().RefreshTimerQueueDeadline(minimumObservedTicks);
    if (!refreshed.HasValue())
    {
        m_localTimerProbeFailure.Controller = refreshed.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRollbackRejected;
        return false;
    }
    m_localTimerProbeFinalDeadlineReceipt = refreshed.GetValue();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Time::Ia64LocalTimerSnapshot localTimer = m_localTimer.GetSnapshot();
    if ((m_localTimerProbeFinalDeadlineReceipt.SelectedDeadlineTicks != 0U) ||
        (m_localTimerProbeFinalDeadlineReceipt.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (m_localTimerProbeFinalDeadlineReceipt.SelectedSources != 0U) ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Idle) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (controller.Deadline.SelectedDeadlineTicks != 0U) || (controller.Deadline.SelectedSources != 0U) ||
        (localTimer.State != Time::Ia64LocalTimerState::Ready) || (localTimer.Armed != 0U) ||
        (localTimer.ProgrammedDeadlineTicks != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRollbackRejected;
        return false;
    }
    return true;
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::ValidateLocalTimerProbeDeliveryPostconditions() const noexcept
{
    const Kernel::Time::TimerSnapshot timer = m_localTimerProbeTimer.GetSnapshot();
    const Kernel::Time::TimerQueueState queue = m_timerQueue.GetState();
    const Kernel::Ipc::NotificationSnapshot notification = m_localTimerProbeNotification.GetSnapshot();
    return m_localTimerProbeDeliveryReceipt.TimerIdentity.IsEqualTo(m_localTimerProbeTimerIdentity) &&
           m_localTimerProbeDeliveryReceipt.NotificationIdentity.IsEqualTo(m_localTimerProbeNotificationIdentity) &&
           m_localTimerProbeDeliveryReceipt.Expiration.Token.IsEqualTo(m_localTimerProbeArmReceipt.Timer.Token) &&
           (m_localTimerProbeDeliveryReceipt.Expiration.DeadlineTicks ==
            m_localTimerProbeFailure.ProgrammedDeadlineTicks) &&
           (m_localTimerProbeDeliveryReceipt.Expiration.ObservedTicks >=
            m_localTimerProbeFailure.ProgrammedDeadlineTicks) &&
           (m_localTimerProbeDeliveryReceipt.ClaimSequence == 1U) &&
           (m_localTimerProbeDeliveryReceipt.NotificationPublished == 1U) &&
           (m_localTimerProbeDeliveryReceipt.QueueCommitted == 1U) &&
           (m_localTimerProbeDeliveryReceipt.QueueState.NextDeadlineTicks == 0U) &&
           (m_localTimerProbeDeliveryReceipt.QueueState.NextClaimSequence == 2U) &&
           (m_localTimerProbeDeliveryReceipt.QueueState.ScheduledEntries == 0U) &&
           (m_localTimerProbeDeliveryReceipt.QueueState.PendingEntries == 0U) &&
           (m_localTimerProbeDeliveryReceipt.QueueState.ClaimedEntries == 0U) &&
           (m_localTimerProbeDeliveryReceipt.NotificationState.State == Kernel::Ipc::NotificationState::Ready) &&
           (m_localTimerProbeDeliveryReceipt.NotificationState.LastClaimSequence == 0U) &&
           (m_localTimerProbeDeliveryReceipt.NotificationState.Pending == 1U) &&
           (timer.State == Kernel::Time::TimerState::Disarmed) && !timer.Token.IsValid() &&
           (timer.DeadlineTicks == 0U) && (queue.NextDeadlineTicks == 0U) && (queue.NextClaimSequence == 2U) &&
           (queue.ScheduledEntries == 0U) && (queue.PendingEntries == 0U) && (queue.ClaimedEntries == 0U) &&
           (notification.State == Kernel::Ipc::NotificationState::Ready) && (notification.Pending == 1U) &&
           (notification.LastClaimSequence == 0U);
}

bool Ia64BspKernelRuntime::DeliverLocalTimerProbeExpiration() noexcept
{
    const auto delivered =
        m_localTimerProbeDelivery.Deliver(m_localTimerProbeTimerIdentity, m_localTimerProbeTimer,
                                          m_localTimerProbeNotificationIdentity, m_localTimerProbeNotification);
    if (!delivered.HasValue())
    {
        m_localTimerProbeFailure.Delivery = delivered.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerDeliveryRejected;
        return false;
    }

    m_localTimerProbeDeliveryReceipt = delivered.GetValue();
    if (!ValidateLocalTimerProbeDeliveryPostconditions())
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::TimerDeliveryRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::ConsumeLocalTimerProbeNotification() noexcept
{
    const auto claimed = m_localTimerProbeNotification.Claim();
    if (!claimed.HasValue())
    {
        m_localTimerProbeFailure.Notification = claimed.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::NotificationClaimRejected;
        return false;
    }
    const Kernel::Ipc::NotificationClaimToken token = claimed.GetValue();
    m_localTimerProbeNotificationClaimSequence = token.GetSequence();
    const Kernel::Ipc::NotificationSnapshot claimedSnapshot = m_localTimerProbeNotification.GetSnapshot();
    if (!token.IsValid() || (token.GetSequence() != 1U) || (claimedSnapshot.LastClaimSequence != token.GetSequence()) ||
        (claimedSnapshot.State != Kernel::Ipc::NotificationState::Claimed) || (claimedSnapshot.Pending != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::NotificationClaimRejected;
        return false;
    }

    const auto committed = m_localTimerProbeNotification.CommitClaim(token);
    if (!committed.HasValue())
    {
        m_localTimerProbeFailure.Notification = committed.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::NotificationCommitRejected;
        return false;
    }
    m_localTimerProbeFinalNotificationSnapshot = committed.GetValue();
    const Kernel::Ipc::NotificationSnapshot finalSnapshot = m_localTimerProbeNotification.GetSnapshot();
    if ((m_localTimerProbeFinalNotificationSnapshot.LastClaimSequence != token.GetSequence()) ||
        (m_localTimerProbeFinalNotificationSnapshot.State != Kernel::Ipc::NotificationState::Ready) ||
        (m_localTimerProbeFinalNotificationSnapshot.Pending != 0U) ||
        (finalSnapshot.LastClaimSequence != token.GetSequence()) ||
        (finalSnapshot.State != Kernel::Ipc::NotificationState::Ready) || (finalSnapshot.Pending != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::NotificationCommitRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::RefreshEmptyLocalTimerProbeDeadline() noexcept
{
    const Kernel::Time::TimerQueueState queue = m_timerQueue.GetState();
    if ((queue.NextDeadlineTicks != 0U) || (queue.NextClaimSequence != 2U) || (queue.ScheduledEntries != 0U) ||
        (queue.PendingEntries != 0U) || (queue.ClaimedEntries != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRefreshRejected;
        return false;
    }

    const auto refreshed =
        Controller().RefreshTimerQueueDeadline(m_localTimerProbeDeliveryReceipt.Expiration.ObservedTicks);
    if (!refreshed.HasValue())
    {
        m_localTimerProbeFailure.Controller = refreshed.GetError();
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRefreshRejected;
        return false;
    }
    m_localTimerProbeFinalDeadlineReceipt = refreshed.GetValue();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Time::Ia64LocalTimerSnapshot localTimer = m_localTimer.GetSnapshot();
    if ((m_localTimerProbeFinalDeadlineReceipt.SelectedDeadlineTicks != 0U) ||
        (m_localTimerProbeFinalDeadlineReceipt.ObservedTicks <
         m_localTimerProbeDeliveryReceipt.Expiration.ObservedTicks) ||
        (m_localTimerProbeFinalDeadlineReceipt.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (m_localTimerProbeFinalDeadlineReceipt.SelectedSources != 0U) ||
        (m_localTimerProbeFinalDeadlineReceipt.Reprogrammed != 1U) ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Idle) ||
        (controller.Deadline.SelectedDeadlineTicks != 0U) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (controller.Deadline.SelectedSources != 0U) || (localTimer.State != Time::Ia64LocalTimerState::Ready) ||
        (localTimer.Vector != m_timerVector) || (localTimer.ProgrammedDeadlineTicks != 0U) || (localTimer.Armed != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::DeadlineRefreshRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::PrepareTimerQueueEvidence() noexcept
{
    m_localTimerProbeTimerQueueEvidence = {};
    const Kernel::Time::TimerSnapshot timer = m_localTimerProbeTimer.GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot notification = m_localTimerProbeNotification.GetSnapshot();
    const Kernel::Time::TimerQueueState queue = m_timerQueue.GetState();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Time::Ia64LocalTimerSnapshot localTimer = m_localTimer.GetSnapshot();
    const Foundation::UInt8 timerQueueSource =
        static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue);
    if ((m_localTimerProbeEvidence.Sequence != 1U) || !m_localTimerProbeTimerIdentity.IsValid() ||
        !m_localTimerProbeNotificationIdentity.IsValid() ||
        !m_localTimerProbeArmReceipt.Timer.Token.IsEqualTo(m_localTimerProbeDeliveryReceipt.Expiration.Token) ||
        (m_localTimerProbeArmReceipt.Timer.Token.GetSequence() != 1U) ||
        (m_localTimerProbeArmReceipt.Timer.DeadlineTicks != m_localTimerProbeEvidence.ProgrammedDeadlineTicks) ||
        (m_localTimerProbeDeliveryReceipt.Expiration.DeadlineTicks !=
         m_localTimerProbeEvidence.ProgrammedDeadlineTicks) ||
        (m_localTimerProbeDeliveryReceipt.Expiration.ObservedTicks <
         m_localTimerProbeEvidence.ObservedIntervalTimeCounter) ||
        !m_localTimerProbeDeliveryReceipt.TimerIdentity.IsEqualTo(m_localTimerProbeTimerIdentity) ||
        !m_localTimerProbeDeliveryReceipt.NotificationIdentity.IsEqualTo(m_localTimerProbeNotificationIdentity) ||
        (m_localTimerProbeDeliveryReceipt.QueueState.NextDeadlineTicks != 0U) ||
        (m_localTimerProbeDeliveryReceipt.QueueState.NextClaimSequence != 2U) ||
        (m_localTimerProbeDeliveryReceipt.QueueState.ScheduledEntries != 0U) ||
        (m_localTimerProbeDeliveryReceipt.QueueState.PendingEntries != 0U) ||
        (m_localTimerProbeDeliveryReceipt.QueueState.ClaimedEntries != 0U) ||
        (m_localTimerProbeInitialDeadlineReceipt.SelectedDeadlineTicks !=
         m_localTimerProbeEvidence.ProgrammedDeadlineTicks) ||
        (m_localTimerProbeInitialDeadlineReceipt.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
        (m_localTimerProbeInitialDeadlineReceipt.SelectedSources != timerQueueSource) ||
        (m_localTimerProbeInitialDeadlineReceipt.Reprogrammed != 1U) ||
        (m_localTimerProbeDeliveryReceipt.ClaimSequence != 1U) || (m_localTimerProbeNotificationClaimSequence != 1U) ||
        (m_localTimerProbeDeliveryReceipt.NotificationState.State != Kernel::Ipc::NotificationState::Ready) ||
        (m_localTimerProbeDeliveryReceipt.NotificationState.LastClaimSequence != 0U) ||
        (m_localTimerProbeDeliveryReceipt.NotificationState.Pending != 1U) ||
        (timer.State != Kernel::Time::TimerState::Disarmed) || timer.Token.IsValid() || (timer.DeadlineTicks != 0U) ||
        (notification.State != Kernel::Ipc::NotificationState::Ready) || (notification.Pending != 0U) ||
        (notification.LastClaimSequence != m_localTimerProbeNotificationClaimSequence) ||
        (m_localTimerProbeFinalDeadlineReceipt.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (m_localTimerProbeFinalDeadlineReceipt.SelectedDeadlineTicks != 0U) ||
        (m_localTimerProbeFinalDeadlineReceipt.SelectedSources != 0U) ||
        (m_localTimerProbeFinalDeadlineReceipt.Reprogrammed != 1U) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (controller.Deadline.SelectedDeadlineTicks != 0U) || (controller.Deadline.SelectedSources != 0U) ||
        (localTimer.State != Time::Ia64LocalTimerState::Ready) || (localTimer.Armed != 0U) ||
        (localTimer.ProgrammedDeadlineTicks != 0U) || (queue.NextDeadlineTicks != 0U) ||
        (queue.NextClaimSequence != 2U) || (queue.ScheduledEntries != 0U) || (queue.PendingEntries != 0U) ||
        (queue.ClaimedEntries != 0U))
    {
        return false;
    }

    m_localTimerProbeTimerQueueEvidence.TimerObjectIdentity = m_localTimerProbeTimerIdentity.GetValue();
    m_localTimerProbeTimerQueueEvidence.NotificationObjectIdentity = m_localTimerProbeNotificationIdentity.GetValue();
    m_localTimerProbeTimerQueueEvidence.TimerArmSequence = m_localTimerProbeArmReceipt.Timer.Token.GetSequence();
    m_localTimerProbeTimerQueueEvidence.DeadlineTicks = m_localTimerProbeDeliveryReceipt.Expiration.DeadlineTicks;
    m_localTimerProbeTimerQueueEvidence.ObservedTicks = m_localTimerProbeDeliveryReceipt.Expiration.ObservedTicks;
    m_localTimerProbeTimerQueueEvidence.QueueClaimSequence = m_localTimerProbeDeliveryReceipt.ClaimSequence;
    m_localTimerProbeTimerQueueEvidence.NotificationClaimSequence = m_localTimerProbeNotificationClaimSequence;
    m_localTimerProbeTimerQueueEvidence.InitialDeadlineState =
        static_cast<Foundation::UInt8>(m_localTimerProbeInitialDeadlineReceipt.State);
    m_localTimerProbeTimerQueueEvidence.SelectedDeadlineSources =
        m_localTimerProbeInitialDeadlineReceipt.SelectedSources;
    m_localTimerProbeTimerQueueEvidence.InitialDeadlineReprogrammed =
        m_localTimerProbeInitialDeadlineReceipt.Reprogrammed;
    m_localTimerProbeTimerQueueEvidence.ClaimedTimerState =
        static_cast<Foundation::UInt8>(Kernel::Time::TimerState::Expired);
    m_localTimerProbeTimerQueueEvidence.NotificationPendingAfterSignal =
        m_localTimerProbeDeliveryReceipt.NotificationState.Pending;
    m_localTimerProbeTimerQueueEvidence.FinalTimerState = static_cast<Foundation::UInt8>(timer.State);
    m_localTimerProbeTimerQueueEvidence.FinalNotificationState = static_cast<Foundation::UInt8>(notification.State);
    m_localTimerProbeTimerQueueEvidence.FinalNotificationPending = notification.Pending;
    m_localTimerProbeTimerQueueEvidence.FinalDeadlineState = static_cast<Foundation::UInt8>(controller.Deadline.State);
    m_localTimerProbeTimerQueueEvidence.FinalLocalTimerState = static_cast<Foundation::UInt8>(localTimer.State);
    m_localTimerProbeTimerQueueEvidence.FinalLocalTimerArmed = localTimer.Armed;
    m_localTimerProbeTimerQueueEvidence.FinalScheduledEntries = queue.ScheduledEntries;
    m_localTimerProbeTimerQueueEvidence.FinalPendingEntries = queue.PendingEntries;
    m_localTimerProbeTimerQueueEvidence.FinalClaimedEntries = queue.ClaimedEntries;
    return true;
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::CompleteLocalTimerProbeAttempt() noexcept
{
    m_localTimerProbePublication = m_runtimeMemory->GetExternalInterruptPublicationSnapshot();
    m_localTimerProbeFailure.Publication = m_localTimerProbePublication.LastError;
    m_localTimerProbeFailure.InterruptVectorTable = m_localTimerProbePublication.LastInterruptVectorTableError;
    if (!m_localTimerProbePublication.Published ||
        (m_localTimerProbePublication.LastError != Memory::Ia64BspRuntimeExternalInterruptPublicationError::None) ||
        (m_localTimerProbePublication.LastInterruptVectorTableError !=
         Memory::Ia64RuntimeExternalInterruptPublicationError::None) ||
        (m_localTimerProbePublication.ContextAddress != m_localTimerProbePublishedContextAddress) ||
        (m_localTimerProbePublication.EvidenceAddress != m_localTimerProbePublishedEvidenceAddress) ||
        (m_localTimerProbePublication.Context.EvidenceAddress != m_localTimerProbePublishedEvidenceAddress) ||
        (m_localTimerProbePublication.Context.ProcessorIndex != m_root.Processor.GetValue()) ||
        (m_localTimerProbePublication.Context.TimerVector != Interrupt::Ia64ExternalInterruptTimerVector) ||
        (m_localTimerProbePublication.Context.TimerVector != m_timerVector) ||
        (m_localTimerProbePublication.Context.ExternalPreemptionControlAddress != 0U))
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::ExternalInterruptPublicationMismatch;
        return false;
    }
    m_localTimerProbeEvidence = m_localTimerProbePublication.Evidence;

    m_localTimerProbePollingRange = Interrupt::GetIa64ExternalInterruptPollCodeRange();
    m_localTimerProbeFailure.EvidenceValidation = Interrupt::ValidateIa64LocalTimerInterruptEvidence(
        m_localTimerProbeEvidence, m_root.Processor.GetValue(), m_localTimerProbeFailure.ProgrammedDeadlineTicks,
        m_localTimerProbePollingRange.Start, m_localTimerProbePollingRange.End);
    if (m_localTimerProbeFailure.EvidenceValidation != Interrupt::Ia64LocalTimerInterruptEvidenceValidationError::None)
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::EvidenceRejected;
        return false;
    }
    if (!PrepareTimerQueueEvidence())
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::PostconditionRejected;
        return false;
    }
    if (!ValidateLocalTimerProbeCommitPostconditions())
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::PostconditionRejected;
        return false;
    }
    // This is the retained transaction's sole commit store. Every semantic
    // and runtime postcondition is final before it, and no such gate follows.
    m_localTimerProbeTimerQueueEvidence.Sequence = m_localTimerProbeEvidence.Sequence;
    m_localTimerProbeFailure.Acknowledgement =
        m_runtimeMemory->AcknowledgeExternalInterruptEvidence(m_localTimerProbeEvidence.Sequence);
    if (m_localTimerProbeFailure.Acknowledgement != Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError::None)
    {
        m_localTimerProbeFailure.Error = Ia64BspKernelRuntimeLocalTimerProbeError::EvidenceAcknowledgementRejected;
        return false;
    }
    m_localTimerProbeEvidence.AcknowledgedSequence = m_localTimerProbeEvidence.Sequence;
    RecordLocalTimerProbeEvidence();
    return true;
}

bool Ia64BspKernelRuntime::PrepareUserProbeWaitPath() noexcept
{
    if ((m_localTimerProbeState != Ia64BspKernelRuntimeLocalTimerProbeState::Complete) ||
        !m_userProbeWaitSetIdentity.IsValid() || m_deferredTimerRegistration.IsValid() ||
        m_userProbeWaitRegistration.IsValid())
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitPathRejected;
        return false;
    }

    const auto deferred =
        m_deferredTimerWorker.Register(m_localTimerProbeTimerIdentity, m_localTimerProbeTimer,
                                       m_localTimerProbeNotificationIdentity, m_localTimerProbeNotification);
    if (!deferred.HasValue())
    {
        m_userProbeFailure.DeferredTimer = deferred.GetErrorReference();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitPathRejected;
        return false;
    }
    m_deferredTimerRegistration = deferred.GetValue().Registration;

    const auto registered = m_userProbeWaitSet.RegisterNotification(
        m_localTimerProbeNotificationIdentity, m_localTimerProbeNotification, Ia64UserExecutionProbeExpectedWaitTag);
    if (!registered.HasValue())
    {
        m_userProbeFailure.WaitSet = registered.GetErrorReference();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitPathRejected;
        return false;
    }
    m_userProbeWaitRegistration = registered.GetValue().Registration;

    const Kernel::Time::DeferredTimerWorkerSnapshot worker = m_deferredTimerWorker.GetSnapshot();
    const Kernel::Ipc::WaitSetSnapshot waitSet = m_userProbeWaitSet.GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot notification = m_localTimerProbeNotification.GetSnapshot();
    if (!m_deferredTimerRegistration.IsValid() || (m_deferredTimerRegistration.GetSequence() != 1U) ||
        !m_userProbeWaitRegistration.IsValid() || (m_userProbeWaitRegistration.GetSequence() != 1U) ||
        !deferred.GetValue().TimerIdentity.IsEqualTo(m_localTimerProbeTimerIdentity) ||
        !deferred.GetValue().NotificationIdentity.IsEqualTo(m_localTimerProbeNotificationIdentity) ||
        !registered.GetValue().Notification.IsEqualTo(m_localTimerProbeNotificationIdentity) ||
        (registered.GetValue().Tag != Ia64UserExecutionProbeExpectedWaitTag) ||
        (worker.State != Kernel::Time::DeferredTimerWorkerState::Ready) || (worker.RegisteredBindings != 1U) ||
        (worker.LastRegistrationSequence != 1U) || (waitSet.State != Kernel::Ipc::WaitSetState::Ready) ||
        (waitSet.RegisteredEntries != 1U) || (waitSet.ClaimedEntries != 0U) ||
        (waitSet.LastRegistrationSequence != 1U) || (waitSet.LastClaimSequence != 0U) ||
        (notification.State != Kernel::Ipc::NotificationState::Ready) || (notification.Pending != 0U) ||
        (notification.LastClaimSequence != 1U))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitPathRejected;
        return false;
    }
    m_userProbeWaitSequence = m_localTimerProbeEvidence.Sequence;
    return true;
}

namespace Detail
{
bool TryComputeUserProbeWaitDeadlines(const Foundation::UInt64 observedTicks, Foundation::UInt64& deadlineTicks,
                                      Foundation::UInt64& timeoutTicks) noexcept
{
    if (observedTicks >= (MaximumUInt64 - Ia64BspUserProbeWaitTimerLeadTicks - Ia64BspLocalTimerProbeGraceTicks))
    {
        return false;
    }
    deadlineTicks = observedTicks + Ia64BspUserProbeWaitTimerLeadTicks;
    timeoutTicks = deadlineTicks + Ia64BspLocalTimerProbeGraceTicks;
    return true;
}
} // Namespace Detail

bool Ia64BspKernelRuntime::ArmUserProbeWaitTimer() noexcept
{
    Foundation::UInt64 nextSequence{0U};
    const Kernel::Time::TimerQueueState queue = m_timerQueue.GetState();
    const Kernel::Time::TimerSnapshot timer = m_localTimerProbeTimer.GetSnapshot();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const System::Ia64SystemCallWaitContinuationSnapshot continuation = m_systemCallWaitContinuation.GetSnapshot();
    if (!m_deferredTimerRegistration.IsValid() || !m_userProbeWaitRegistration.IsValid() ||
        (m_userProbeWaitSequence == 0U) || (m_userProbeWaitSequence >= (MaximumUInt64 - 1U)) ||
        (queue.NextDeadlineTicks != 0U) || (queue.ScheduledEntries != 0U) || (queue.PendingEntries != 0U) ||
        (queue.ClaimedEntries != 0U) || (timer.State != Kernel::Time::TimerState::Disarmed) || timer.Token.IsValid() ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Active) ||
        (controller.Mode != Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) ||
        (continuation.State != System::Ia64SystemCallWaitContinuationState::SupervisorActive))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitTimerRejected;
        return false;
    }
    nextSequence = m_userProbeWaitSequence + 1U;
    if (queue.NextClaimSequence != nextSequence)
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitTimerRejected;
        return false;
    }

    const Foundation::UInt64 now = m_localTimer.GetTicks();
    Foundation::UInt64 deadlineTicks{0U};
    Foundation::UInt64 timeoutTicks{0U};
    if (!Detail::TryComputeUserProbeWaitDeadlines(now, deadlineTicks, timeoutTicks))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitTimerRejected;
        return false;
    }
    m_userProbeWaitDeadlineTicks = deadlineTicks;
    m_userProbeWaitTimeoutTicks = timeoutTicks;

    const auto armed = m_timerQueue.Arm(m_localTimerProbeTimerIdentity, m_localTimerProbeTimer,
                                        Kernel::Object::MonotonicTime{m_userProbeWaitDeadlineTicks});
    if (!armed.HasValue())
    {
        m_localTimerProbeFailure.TimerQueue = armed.GetErrorReference();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitTimerRejected;
        return false;
    }
    m_userProbeWaitArmReceipt = armed.GetValue();
    const Kernel::Time::TimerQueueState armedQueue = m_timerQueue.GetState();
    if (!m_userProbeWaitArmReceipt.Timer.Token.IsValid() ||
        (m_userProbeWaitArmReceipt.Timer.Token.GetSequence() != nextSequence) ||
        m_userProbeWaitArmReceipt.Timer.Replaced ||
        (m_userProbeWaitArmReceipt.Timer.DeadlineTicks != m_userProbeWaitDeadlineTicks) ||
        (m_userProbeWaitArmReceipt.NextDeadlineTicks != m_userProbeWaitDeadlineTicks) ||
        (armedQueue.NextDeadlineTicks != m_userProbeWaitDeadlineTicks) ||
        (armedQueue.NextClaimSequence != nextSequence) || (armedQueue.ScheduledEntries != 1U) ||
        (armedQueue.PendingEntries != 0U) || (armedQueue.ClaimedEntries != 0U))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitTimerRejected;
        return false;
    }
    m_userProbeWaitSequence = nextSequence;
    return true;
}

void Ia64BspKernelRuntime::RecordUserProbeWaitTimerEvidence() const noexcept
{
    Platform::Ia64::Diagnostics::Ia64UserProbeEvidence& evidence =
        Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().UserProbe;
    evidence.WaitTimerInterrupt = m_userProbeWaitTimerEvidence;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.WaitTimerPublished);
}

bool Ia64BspKernelRuntime::ValidateUserProbeWaitSuspension() const noexcept
{
    const Foundation::UInt8 timerQueueSource =
        static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::TimerQueue);
    if ((m_dispatchFailure.Error != Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected) ||
        (m_dispatchFailure.ControllerError != Kernel::Scheduler::ProcessorRunError::NoRunnableThread) ||
        (m_dispatchFailure.SchedulerError != Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread) ||
        (m_dispatchFailure.DeadlineError != Kernel::Time::ProcessorDeadlineError::None) ||
        (m_dispatchFailure.DeadlineReceipt.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
        (m_dispatchFailure.DeadlineReceipt.SelectedDeadlineTicks != m_userProbeWaitDeadlineTicks) ||
        (m_dispatchFailure.DeadlineReceipt.SelectedSources != timerQueueSource) ||
        (m_dispatchFailure.ActiveRunRetained != 0U) || (m_userProbeWaitSequence < 2U))
    {
        return false;
    }
    {
        const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
        if ((controller.State != Kernel::Scheduler::ProcessorRunState::Idle) ||
            (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
            (controller.Deadline.SelectedDeadlineTicks != m_userProbeWaitDeadlineTicks))
        {
            return false;
        }
    }
    {
        const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = RunLoop().GetSnapshot();
        if (runLoop.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle)
        {
            return false;
        }
    }
    {
        const Kernel::Ipc::WaitSetThreadDispatcherSnapshot wait = m_waitSetThreadDispatcher->GetSnapshot();
        if (wait.State != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked)
        {
            return false;
        }
    }
    {
        const Kernel::Scheduler::ThreadSnapshot thread = m_thread.GetSnapshot();
        if (thread.State != Kernel::Scheduler::ThreadState::Blocked)
        {
            return false;
        }
    }
    if (!ValidateBorrowedInitialComponentRoot())
    {
        return false;
    }
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot addressSpaceController =
        m_root.ControllerStorage->GetSnapshot();
    if ((addressSpaceController.ActiveStorage != m_root.AddressSpaceStorage) ||
        !addressSpaceController.Active.IsEqualTo(m_root.AddressSpaceIdentity) ||
        (m_userRoot.AddressSpaceStorage->GetSnapshot().ResidentProcessorMask != 0U))
    {
        return false;
    }
    const System::Ia64SystemCallWaitContinuationSnapshot continuation = m_systemCallWaitContinuation.GetSnapshot();
    return continuation.State == System::Ia64SystemCallWaitContinuationState::WaitingReadmission;
}

bool Ia64BspKernelRuntime::PollUserProbeWaitInterrupt() noexcept
{
    m_userProbeFailure.WaitPoll = AsteriaIa64PollForExternalInterruptSequence(
        m_localTimerProbePublishedEvidenceAddress, m_userProbeWaitSequence, m_userProbeWaitTimeoutTicks);
    if (m_userProbeFailure.WaitPoll != Interrupt::Ia64ExternalInterruptPollStatus::Completed)
    {
        return false;
    }

    const Memory::Ia64BspRuntimeExternalInterruptPublicationSnapshot publication =
        m_runtimeMemory->GetExternalInterruptPublicationSnapshot();
    m_userProbeWaitTimerEvidence = publication.Evidence;
    m_userProbeFailure.WaitEvidenceValidation = Interrupt::ValidateIa64LocalTimerInterruptEvidenceForSequence(
        m_userProbeWaitTimerEvidence, m_userProbeWaitSequence, m_root.Processor.GetValue(),
        m_userProbeWaitDeadlineTicks, m_localTimerProbePollingRange.Start, m_localTimerProbePollingRange.End);
    const bool evidenceIsValid =
        publication.Published && (publication.EvidenceAddress == m_localTimerProbePublishedEvidenceAddress) &&
        (publication.ContextAddress == m_localTimerProbePublishedContextAddress) &&
        (m_userProbeFailure.WaitEvidenceValidation == Interrupt::Ia64LocalTimerInterruptEvidenceValidationError::None);
    if (!evidenceIsValid)
    {
        return false;
    }
    RecordUserProbeWaitTimerEvidence();
    return true;
}

bool Ia64BspKernelRuntime::DeliverUserProbeWaitTimer() noexcept
{
    if (m_deferredTimerWorker.DrainDueRetained() != Kernel::Time::DeferredTimerDrainOutcome::Drained)
    {
        m_userProbeFailure.DeferredTimer = m_deferredTimerWorker.GetRetainedDrainFailure();
        return false;
    }
    const Kernel::Time::DeferredTimerDrainReceipt& drained = m_deferredTimerWorker.GetRetainedDrainReceipt();
    return (drained.Stop == Kernel::Time::DeferredTimerDrainStop::Empty) && (drained.DeliveredCount == 1U) &&
           drained.FirstTimerIdentity.IsEqualTo(m_localTimerProbeTimerIdentity) &&
           drained.LastTimerIdentity.IsEqualTo(m_localTimerProbeTimerIdentity) &&
           (drained.FirstClaimSequence == m_userProbeWaitSequence) &&
           (drained.LastClaimSequence == m_userProbeWaitSequence) && (drained.QueueState.NextDeadlineTicks == 0U) &&
           (drained.QueueState.NextClaimSequence == m_userProbeWaitSequence + 1U) &&
           (drained.QueueState.ScheduledEntries == 0U) && (drained.QueueState.PendingEntries == 0U) &&
           (drained.QueueState.ClaimedEntries == 0U);
}

bool Ia64BspKernelRuntime::RefreshUserProbeWaitDeadline() noexcept
{
    const Kernel::Time::TimerNotificationDeliveryReceipt& delivery = m_deferredTimerWorker.GetLastDeliveryReceipt();
    const auto deadline = Controller().RefreshTimerQueueDeadline(delivery.Expiration.ObservedTicks);
    if (!deadline.HasValue())
    {
        m_userProbeFailure.WaitController = deadline.GetErrorReference();
        return false;
    }
    return (deadline.GetValue().State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (deadline.GetValue().SelectedDeadlineTicks == 0U) && (deadline.GetValue().SelectedSources == 0U);
}

bool Ia64BspKernelRuntime::WakeUserProbeWaitThread() noexcept
{
    const auto woke = m_waitSetThreadDispatcher->Poll();
    if (!woke.HasValue())
    {
        m_userProbeFailure.WaitDispatcher = woke.GetErrorReference();
        return false;
    }
    const Kernel::Ipc::WaitSetThreadDispatcherReceipt& wake = woke.GetValue();
    return (wake.State == Kernel::Ipc::WaitSetThreadDispatcherState::Completed) &&
           wake.WaitSet.IsEqualTo(m_userProbeWaitSetIdentity) && wake.Thread.IsEqualTo(m_threadIdentity) &&
           wake.SchedulingContext.IsEqualTo(m_schedulingContextIdentity);
}

bool Ia64BspKernelRuntime::CompleteUserProbeWaitFromIdle() noexcept
{
    if (!ValidateUserProbeWaitSuspension())
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSuspendRejected;
        return false;
    }
    if (!m_initialComponentWaitIdleStageReported)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:IA64-PRIVATE-USER-ROOT-WAIT-IDLE\r\n");
        m_initialComponentWaitIdleStageReported = true;
    }
    if (!PollUserProbeWaitInterrupt())
    {
        if (m_userProbeFailure.Error == Ia64BspKernelRuntimeUserProbeError::None)
        {
            m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitInterruptRejected;
        }
        return false;
    }
    if (!DeliverUserProbeWaitTimer() || !RefreshUserProbeWaitDeadline())
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitDeliveryRejected;
        return false;
    }
    if (!WakeUserProbeWaitThread())
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitWakeRejected;
        return false;
    }

    m_userProbeFailure.WaitAcknowledgement =
        m_runtimeMemory->AcknowledgeExternalInterruptEvidence(m_userProbeWaitSequence);
    if (m_userProbeFailure.WaitAcknowledgement != Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError::None)
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitInterruptRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::ConsumeUserProbeWaitCompletion() noexcept
{
    m_userProbeWaitCompletion = {};
    m_userProbeFailure.WaitIngress = {};
    if (!Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(
            RunLoop(), *m_waitSetSystemCallDispatcher, m_userProbeWaitCompletion, m_userProbeFailure.WaitIngress))
    {
        return false;
    }
    return (m_userProbeWaitCompletion.Disposition == Kernel::System::WaitSetSystemCallDisposition::Completed) &&
           (m_userProbeWaitCompletion.Wait.State == Kernel::Ipc::WaitSetThreadDispatcherState::Completed);
}

bool Ia64BspKernelRuntime::PrepareUserProbeSystemCallResume() noexcept
{
    const auto active = RunLoop().GetActiveExecutionBinding();
    if (!active.HasValue() || (m_systemCallEntryFrame == nullptr) || !ValidateBorrowedInitialComponentRoot() ||
        (m_root.ControllerStorage->GetSnapshot().ActiveStorage != m_userRoot.AddressSpaceStorage))
    {
        return false;
    }
    const Kernel::Scheduler::ProcessorActiveExecutionBinding& binding = active.GetValue();
    const System::Ia64SystemCallWaitCorrelation correlation{
        .Thread = binding.ThreadIdentity,
        .SchedulingContext = binding.SchedulingContextIdentity,
        .RunSequence = binding.RunSequence,
        .Processor = binding.Processor,
    };
    m_userProbeFailure.WaitContinuation = m_systemCallWaitContinuation.PrepareSystemCallResume(
        *m_systemCallEntryFrame, correlation, m_userProbeWaitCompletion.Response);
    if (m_userProbeFailure.WaitContinuation != System::Ia64SystemCallWaitContinuationError::None)
    {
        return false;
    }
    if (!m_initialComponentResumeStageReported)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:IA64-PRIVATE-USER-ROOT-RESUME\r\n");
        m_initialComponentResumeStageReported = true;
    }
    return true;
}

System::Ia64SystemCallWaitSupervisorReceipt Ia64BspKernelRuntime::ResumeUserProbeSystemCall() noexcept
{
    if (!ConsumeUserProbeWaitCompletion() || !PrepareUserProbeSystemCallResume())
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitResumeRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::WaitCompletion);
    }

    const System::Ia64SystemCallWaitSupervisorResult resumed = m_systemCallWaitContinuation.ResumeSystemCall();
    if (!resumed.HasValue())
    {
        m_userProbeFailure.WaitContinuation = resumed.GetError().Error;
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitResumeRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::WaitResume);
    }
    return resumed.GetValue();
}

bool Ia64BspKernelRuntime::ValidatePlacementAndMapping() const noexcept
{
    const Foundation::UInt64 start = AddressOf(this);
    Foundation::UInt64 end{0U};
    if (((start & PageOffsetMask) != 0U) || !TryGetEnd(start, sizeof(*this), end) || (end > RegionZeroLimit) ||
        (m_root.AddressSpaceStorage == nullptr))
    {
        return false;
    }
    return MappingContainsWritableRange(*m_root.AddressSpaceStorage, start, end);
}

bool Ia64BspKernelRuntime::ValidateBorrowedRoot() const noexcept
{
    if ((m_root.AddressSpaceStorage == nullptr) || (m_root.ControllerStorage == nullptr) ||
        !m_root.AddressSpaceIdentity.IsValid() || !m_root.AddressSpaceIdentity.IsFromRegistry(m_objectRegistry) ||
        !m_root.Processor.IsValid())
    {
        return false;
    }

    const Kernel::Memory::AddressSpaceSnapshot addressSpace = m_root.AddressSpaceStorage->GetSnapshot();
    if ((addressSpace.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !addressSpace.Identity.IsEqualTo(m_root.AddressSpaceIdentity))
    {
        return false;
    }

    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controller = m_root.ControllerStorage->GetSnapshot();
    return (controller.State == Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) &&
           controller.Processor.IsValid() && (controller.Processor.GetValue() == m_root.Processor.GetValue()) &&
           (controller.IdleStorage == m_root.AddressSpaceStorage) &&
           (controller.ActiveStorage == m_root.AddressSpaceStorage) &&
           controller.Idle.IsEqualTo(m_root.AddressSpaceIdentity) &&
           controller.Active.IsEqualTo(m_root.AddressSpaceIdentity);
}

bool Ia64BspKernelRuntime::ValidateBorrowedInitialComponentRoot() const noexcept
{
    if ((m_root.AddressSpaceStorage == nullptr) || (m_root.ControllerStorage == nullptr) ||
        (m_userRoot.AddressSpaceStorage == nullptr) || (m_userRoot.ControllerStorage != m_root.ControllerStorage) ||
        !m_root.AddressSpaceIdentity.IsValid() || !m_userRoot.AddressSpaceIdentity.IsValid() ||
        m_userRoot.AddressSpaceIdentity.IsEqualTo(m_root.AddressSpaceIdentity) ||
        !m_root.AddressSpaceIdentity.IsFromRegistry(m_objectRegistry) ||
        !m_userRoot.AddressSpaceIdentity.IsFromRegistry(m_objectRegistry) || !m_root.Processor.IsValid() ||
        !m_userRoot.Processor.IsValid() || (m_userRoot.Processor.GetValue() != m_root.Processor.GetValue()))
    {
        return false;
    }

    const Foundation::UInt8 processorBit =
        static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << m_root.Processor.GetValue());
    const Kernel::Memory::AddressSpaceSnapshot common = m_root.AddressSpaceStorage->GetSnapshot();
    const Kernel::Memory::AddressSpaceSnapshot initialComponent = m_userRoot.AddressSpaceStorage->GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controller = m_root.ControllerStorage->GetSnapshot();
    if ((common.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !common.Identity.IsEqualTo(m_root.AddressSpaceIdentity) ||
        (initialComponent.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !initialComponent.Identity.IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        (controller.State != Kernel::Memory::ProcessorAddressSpaceControllerState::Ready) ||
        !controller.Processor.IsValid() || (controller.Processor.GetValue() != m_root.Processor.GetValue()) ||
        (controller.IdleStorage != m_root.AddressSpaceStorage) ||
        !controller.Idle.IsEqualTo(m_root.AddressSpaceIdentity) || (controller.PreviousStorage != nullptr) ||
        controller.Previous.IsValid() || controller.SourceTransition.IsValid() ||
        controller.DestinationTransition.IsValid())
    {
        return false;
    }

    const bool commonActive = (controller.ActiveStorage == m_root.AddressSpaceStorage) &&
                              controller.Active.IsEqualTo(m_root.AddressSpaceIdentity) &&
                              ((common.ResidentProcessorMask & processorBit) != 0U) &&
                              (initialComponent.ResidentProcessorMask == 0U);
    const bool initialComponentActive = (controller.ActiveStorage == m_userRoot.AddressSpaceStorage) &&
                                        controller.Active.IsEqualTo(m_userRoot.AddressSpaceIdentity) &&
                                        ((common.ResidentProcessorMask & processorBit) == 0U) &&
                                        (initialComponent.ResidentProcessorMask == processorBit);
    return commonActive || initialComponentActive;
}

bool Ia64BspKernelRuntime::SwitchToInitialComponentRoot() noexcept
{
    if (!ValidateBorrowedInitialComponentRoot())
    {
        return false;
    }
    const auto switched = m_root.ControllerStorage->SwitchTo(*m_userRoot.AddressSpaceStorage);
    if (!switched.HasValue())
    {
        m_failure.AddressSpaceControllerError = switched.GetError().Error;
        m_userProbeFailure.AddressSpaceController = switched.GetError().Error;
        return false;
    }
    const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& receipt = switched.GetValue();
    return receipt.Previous.IsEqualTo(m_root.AddressSpaceIdentity) &&
           receipt.Active.IsEqualTo(m_userRoot.AddressSpaceIdentity) && receipt.Processor.IsValid() &&
           (receipt.Processor.GetValue() == m_root.Processor.GetValue()) && ValidateBorrowedInitialComponentRoot() &&
           (m_root.ControllerStorage->GetSnapshot().ActiveStorage == m_userRoot.AddressSpaceStorage);
}

bool Ia64BspKernelRuntime::SwitchToCommonRoot() noexcept
{
    if (!ValidateBorrowedInitialComponentRoot())
    {
        return false;
    }
    const auto switched = m_root.ControllerStorage->SwitchToIdle();
    if (!switched.HasValue())
    {
        m_failure.AddressSpaceControllerError = switched.GetError().Error;
        m_userProbeFailure.AddressSpaceController = switched.GetError().Error;
        return false;
    }
    const Kernel::Memory::ProcessorAddressSpaceControllerReceipt& receipt = switched.GetValue();
    return receipt.Previous.IsEqualTo(m_userRoot.AddressSpaceIdentity) &&
           receipt.Active.IsEqualTo(m_root.AddressSpaceIdentity) && receipt.Processor.IsValid() &&
           (receipt.Processor.GetValue() == m_root.Processor.GetValue()) && ValidateBorrowedInitialComponentRoot() &&
           (m_root.ControllerStorage->GetSnapshot().ActiveStorage == m_root.AddressSpaceStorage);
}

bool Ia64BspKernelRuntime::ValidateResourceDomain(const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                  Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(m_objectRegistry))
    {
        return false;
    }
    const Kernel::Resource::ResourceDomainSnapshot snapshot = resourceDomain.GetSnapshot();
    return (snapshot.State == Kernel::Resource::ResourceDomainState::Ready) && snapshot.Identity.IsValid() &&
           (snapshot.Depth == 0U) && !snapshot.Parent.IsValid() &&
           RegistrationMatches(resourceDomainIdentity.GetObjectId(), Kernel::Object::ObjectType::ResourceDomain,
                               resourceDomain);
}

bool Ia64BspKernelRuntime::RegistrationMatches(const Kernel::Object::ObjectId identity,
                                               const Kernel::Object::ObjectType type,
                                               Kernel::Object::IKernelObject& storage) noexcept
{
    Kernel::Object::ObjectPin pin;
    const Kernel::Object::ObjectPinError pinError = m_objectRegistry.PinObject(identity, type, pin);
    m_failure.ObjectPinError = pinError;
    if (pinError != Kernel::Object::ObjectPinError::Ok)
    {
        return false;
    }
    const bool matches = &pin.GetObject() == &storage;
    pin.Reset();
    return matches;
}

bool Ia64BspKernelRuntime::InitializeLocalTimerProbeObjects(const Kernel::Object::ResourceDomainId chargeOwner,
                                                            Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    m_failure.TimerError = m_localTimerProbeTimer.Initialize(chargeOwner);
    if (m_failure.TimerError != Kernel::Time::TimerError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::TimerInitializationRejected;
        return false;
    }

    const auto registeredTimer = issuer.CreateObject(m_localTimerProbeTimer);
    if (!registeredTimer.HasValue())
    {
        m_failure.ObjectRegistryError = registeredTimer.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::TimerRegistrationRejected;
        return false;
    }
    if (!RegistrationMatches(registeredTimer.GetValue(), Kernel::Object::ObjectType::Timer, m_localTimerProbeTimer))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::TimerRegistrationRejected;
        return false;
    }
    m_localTimerProbeTimerIdentity = Kernel::Time::TimerId{m_objectRegistry, registeredTimer.GetValue()};

    m_failure.NotificationError = m_localTimerProbeNotification.Initialize(chargeOwner);
    if (m_failure.NotificationError != Kernel::Ipc::NotificationError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NotificationInitializationRejected;
        return false;
    }

    const auto registeredNotification = issuer.CreateObject(m_localTimerProbeNotification);
    if (!registeredNotification.HasValue())
    {
        m_failure.ObjectRegistryError = registeredNotification.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::NotificationRegistrationRejected;
        return false;
    }
    if (!RegistrationMatches(registeredNotification.GetValue(), Kernel::Object::ObjectType::Notification,
                             m_localTimerProbeNotification))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NotificationRegistrationRejected;
        return false;
    }
    m_localTimerProbeNotificationIdentity =
        Kernel::Ipc::NotificationId{m_objectRegistry, registeredNotification.GetValue()};
    return true;
}

bool Ia64BspKernelRuntime::InitializeUserProbeWaitSet(const Kernel::Object::ResourceDomainId chargeOwner,
                                                      Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    m_failure.WaitSetError = m_userProbeWaitSet.Initialize(chargeOwner);
    if (m_failure.WaitSetError != Kernel::Ipc::WaitSetError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::WaitSetInitializationRejected;
        return false;
    }

    const auto registered = issuer.CreateObject(m_userProbeWaitSet);
    if (!registered.HasValue())
    {
        m_failure.ObjectRegistryError = registered.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::WaitSetRegistrationRejected;
        return false;
    }
    if (!RegistrationMatches(registered.GetValue(), Kernel::Object::ObjectType::WaitSet, m_userProbeWaitSet))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::WaitSetRegistrationRejected;
        return false;
    }
    m_userProbeWaitSetIdentity = Kernel::Ipc::WaitSetId{m_objectRegistry, registered.GetValue()};
    return true;
}

bool Ia64BspKernelRuntime::InitializeProtectionDomain(
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    m_protectionDomain.SetDiagnosticComponentKind(Abi::WatchdogInitialComponent);
    const Kernel::Protection::ProtectionDomainFailure initialized = m_protectionDomain.Initialize(
        m_objectRegistry, resourceDomainIdentity, resourceDomain, m_userRoot.AddressSpaceIdentity, m_capabilitySpace);
    m_failure.ProtectionDomainError = initialized.Error;
    m_failure.ObjectPinError = initialized.Pin;
    if (initialized.Error != Kernel::Protection::ProtectionDomainError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::ProtectionDomainInitializationRejected;
        return false;
    }

    const auto registered = issuer.CreateObject(m_protectionDomain);
    if (!registered.HasValue())
    {
        m_failure.ObjectRegistryError = registered.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::ProtectionDomainRegistrationRejected;
        return false;
    }
    if (!RegistrationMatches(registered.GetValue(), Kernel::Object::ObjectType::ProtectionDomain, m_protectionDomain))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::ProtectionDomainRegistrationRejected;
        return false;
    }

    m_protectionDomainIdentity = Kernel::Protection::ProtectionDomainId{m_objectRegistry, registered.GetValue()};
    const Kernel::Protection::ProtectionDomainFailure bound =
        m_protectionDomain.BindIdentity(m_objectRegistry, m_protectionDomainIdentity);
    m_failure.ProtectionDomainError = bound.Error;
    m_failure.ObjectPinError = bound.Pin;
    if (bound.Error != Kernel::Protection::ProtectionDomainError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::ProtectionDomainIdentityRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::InitializeThread(const Kernel::Object::ResourceDomainId chargeOwner,
                                            Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    m_failure.ThreadError = m_thread.Initialize(chargeOwner);
    if (m_failure.ThreadError != Kernel::Scheduler::ThreadError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::ThreadInitializationRejected;
        return false;
    }

    const auto registered = issuer.CreateObject(m_thread);
    if (!registered.HasValue())
    {
        m_failure.ObjectRegistryError = registered.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::ThreadRegistrationRejected;
        return false;
    }
    if (!RegistrationMatches(registered.GetValue(), Kernel::Object::ObjectType::Thread, m_thread))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::ThreadRegistrationRejected;
        return false;
    }
    m_threadIdentity = Kernel::Scheduler::ThreadId{m_objectRegistry, registered.GetValue()};
    return true;
}

bool Ia64BspKernelRuntime::InitializeSchedulingContext(const Kernel::Object::ResourceDomainId chargeOwner,
                                                       Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    const Foundation::UInt64 initialTicks = m_localTimer.GetTicks();
    if (initialTicks >= (MaximumUInt64 - 1U))
    {
        m_failure.SchedulingContextError = Kernel::Scheduler::SchedulingContextError::InvalidConfiguration;
        m_failure.Error = Ia64BspKernelRuntimeError::SchedulingContextInitializationRejected;
        return false;
    }
    const Foundation::ProcessorMask affinityMask =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << m_root.Processor.GetValue());
#if !ASTERIA_IA64_BSP_USER_TIME_SLICE
    const Foundation::UInt64 windowTicks = MaximumUInt64 - initialTicks - 1U;
    const Kernel::Scheduler::SchedulingContextConfiguration configuration{
        .BudgetTicks = windowTicks,
        .PeriodTicks = windowTicks,
        .RelativeDeadlineTicks = windowTicks,
        .Weight = 1U,
        .Priority = 0xFFU,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Emergency,
        .CpuAffinityMask = affinityMask,
        .Class = Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
        .Reserved = 0U,
    };
#else
    const Kernel::Scheduler::SchedulingContextConfiguration configuration =
        System::MakeIa64UserSchedulingConfiguration(affinityMask);
#endif
    m_failure.SchedulingContextError =
        m_schedulingContext.Initialize(configuration, chargeOwner, Kernel::Object::MonotonicTime{initialTicks});
    if (m_failure.SchedulingContextError != Kernel::Scheduler::SchedulingContextError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::SchedulingContextInitializationRejected;
        return false;
    }

    const auto registered = issuer.CreateObject(m_schedulingContext);
    if (!registered.HasValue())
    {
        m_failure.ObjectRegistryError = registered.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::SchedulingContextRegistrationRejected;
        return false;
    }
    if (!RegistrationMatches(registered.GetValue(), Kernel::Object::ObjectType::SchedulingContext, m_schedulingContext))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::SchedulingContextRegistrationRejected;
        return false;
    }
    m_schedulingContextIdentity = Kernel::Scheduler::SchedulingContextId{m_objectRegistry, registered.GetValue()};
    m_failure.SchedulingContextError = m_schedulingContext.Bind(m_threadIdentity);
    if (m_failure.SchedulingContextError != Kernel::Scheduler::SchedulingContextError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::SchedulingContextBindingRejected;
        return false;
    }
    return true;
}

#if ASTERIA_IA64_BSP_NATIVE_PREEMPTION_PROBE_CODE
bool Ia64BspKernelRuntime::InitializeNativePreemptionSchedulingContext() noexcept
{
    if ((m_resourceDomain == nullptr) || (m_issuer == nullptr) || !m_threadIdentity.IsValid() ||
        m_nativePreemptionSchedulingContextIdentity.IsValid())
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionSchedulingContextRejected;
        return false;
    }

    const Foundation::UInt64 initialTicks = m_localTimer.GetTicks();
    if ((initialTicks == MaximumUInt64) || (initialTicks > (MaximumUInt64 - Ia64BspNativePreemptionPeriodTicks)))
    {
        m_failure.SchedulingContextError = Kernel::Scheduler::SchedulingContextError::InvalidConfiguration;
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionSchedulingContextRejected;
        return false;
    }

    const Foundation::ProcessorMask affinityMask =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << m_root.Processor.GetValue());
    const Kernel::Scheduler::SchedulingContextConfiguration configuration =
        MakeIa64BspNativePreemptionSchedulingConfiguration(affinityMask);
    m_failure.SchedulingContextError = m_nativePreemptionSchedulingContext.Initialize(
        configuration, m_resourceDomain->GetIdentifier(), Kernel::Object::MonotonicTime{initialTicks});
    if (m_failure.SchedulingContextError != Kernel::Scheduler::SchedulingContextError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionSchedulingContextRejected;
        return false;
    }

    const auto registered = m_issuer->CreateObject(m_nativePreemptionSchedulingContext);
    if (!registered.HasValue())
    {
        m_failure.ObjectRegistryError = registered.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionSchedulingContextRejected;
        return false;
    }
    if (!RegistrationMatches(registered.GetValue(), Kernel::Object::ObjectType::SchedulingContext,
                             m_nativePreemptionSchedulingContext))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionSchedulingContextRejected;
        return false;
    }
    m_nativePreemptionSchedulingContextIdentity =
        Kernel::Scheduler::SchedulingContextId{m_objectRegistry, registered.GetValue()};
    m_failure.SchedulingContextError = m_nativePreemptionSchedulingContext.Bind(m_threadIdentity);
    if (m_failure.SchedulingContextError != Kernel::Scheduler::SchedulingContextError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionSchedulingContextRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::ResolveNativePreemptionDescriptors() noexcept
{
    alignas(Foundation::UInt64) Foundation::UInt64 descriptor[6U]{};
    AsteriaIa64InitializeBspNativePreemptionDescriptor(descriptor);
    m_nativePreemptionCoordinatorEntryPoint = descriptor[0U];
    m_nativePreemptionCoordinatorGlobalPointer = descriptor[1U];
    m_nativePreemptionSentinelEntryPoint = descriptor[2U];
    m_nativePreemptionSentinelEnd = descriptor[3U];
    m_nativePreemptionIdleDescriptor[0U] = descriptor[4U];
    m_nativePreemptionIdleDescriptor[1U] = descriptor[5U];
    return (m_nativePreemptionCoordinatorEntryPoint != 0U) && ((m_nativePreemptionCoordinatorEntryPoint & 15U) == 0U) &&
           (m_nativePreemptionSentinelEntryPoint != 0U) && ((m_nativePreemptionSentinelEntryPoint & 15U) == 0U) &&
           ((m_nativePreemptionSentinelEnd - m_nativePreemptionSentinelEntryPoint) == 16U);
}

bool Ia64BspKernelRuntime::PrepareNativePreemptionCorridor() noexcept
{
    if ((m_runtimeMemory == nullptr) || (m_nativePreemptionControl != nullptr) || !m_root.Processor.IsValid() ||
        (m_root.Processor.GetValue() != 0U))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionStorageRejected;
        return false;
    }

    const auto borrowed = m_runtimeMemory->BorrowExternalPreemptionStorage(m_root.Processor);
    if (!borrowed.HasValue())
    {
        m_failure.ExternalPreemptionStorageError = borrowed.GetError();
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionStorageRejected;
        return false;
    }
    m_nativePreemptionStorage = borrowed.GetValue();
    m_nativePreemptionControl = m_nativePreemptionStorage.Control;
    if ((m_nativePreemptionControl == nullptr) || (m_nativePreemptionStorage.CoordinatorMemoryStack == nullptr) ||
        (m_nativePreemptionStorage.CoordinatorRseBackingStore == nullptr) ||
        (m_nativePreemptionStorage.CoordinatorMemoryStackAddress !=
         AddressOf(m_nativePreemptionStorage.CoordinatorMemoryStack)) ||
        (m_nativePreemptionStorage.CoordinatorRseBackingStoreAddress !=
         AddressOf(m_nativePreemptionStorage.CoordinatorRseBackingStore)) ||
        (m_nativePreemptionStorage.CoordinatorMemoryStackSizeBytes !=
         Interrupt::Ia64ExternalPreemptionCoordinatorStackSizeBytes) ||
        (m_nativePreemptionStorage.CoordinatorRseBackingStoreSizeBytes !=
         Interrupt::Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionStorageRejected;
        return false;
    }

    Foundation::UInt64 workerMemoryStackEnd{0U};
    Foundation::UInt64 workerRseBackingStoreEnd{0U};
    Foundation::UInt64 coordinatorMemoryStackEnd{0U};
    Foundation::UInt64 coordinatorRseBackingStoreEnd{0U};
    const Foundation::UInt64 workerMemoryStackStart = AddressOf(m_workerMemoryStack);
    const Foundation::UInt64 workerRseBackingStoreStart = AddressOf(m_workerRseBackingStore);
    if (!ResolveNativePreemptionDescriptors() ||
        !TryGetEnd(workerMemoryStackStart, sizeof(m_workerMemoryStack), workerMemoryStackEnd) ||
        !TryGetEnd(workerRseBackingStoreStart, sizeof(m_workerRseBackingStore), workerRseBackingStoreEnd) ||
        !TryGetEnd(m_nativePreemptionStorage.CoordinatorMemoryStackAddress,
                   m_nativePreemptionStorage.CoordinatorMemoryStackSizeBytes, coordinatorMemoryStackEnd) ||
        !TryGetEnd(m_nativePreemptionStorage.CoordinatorRseBackingStoreAddress,
                   m_nativePreemptionStorage.CoordinatorRseBackingStoreSizeBytes, coordinatorRseBackingStoreEnd))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionControlRejected;
        return false;
    }

    m_failure.ExternalPreemptionContinuationError = m_idleContext.ConfigureExternalFrameIdleContinuation(
        m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit,
        WritableIa64KernelStorageView{
            .Data = m_nativePreemptionStorage.CoordinatorMemoryStack,
            .SizeBytes = m_nativePreemptionStorage.CoordinatorMemoryStackSizeBytes,
            .VirtualStart = m_nativePreemptionStorage.CoordinatorMemoryStackAddress,
        },
        WritableIa64KernelStorageView{
            .Data = m_nativePreemptionStorage.CoordinatorRseBackingStore,
            .SizeBytes = m_nativePreemptionStorage.CoordinatorRseBackingStoreSizeBytes,
            .VirtualStart = m_nativePreemptionStorage.CoordinatorRseBackingStoreAddress,
        },
        Ia64KernelContextResolvedEntry{
            .DescriptorAddress = AddressOf(m_nativePreemptionIdleDescriptor),
            .EntryPointAddress = m_nativePreemptionIdleDescriptor[0U],
            .GlobalPointer = m_nativePreemptionIdleDescriptor[1U],
        },
        this);
    if (m_failure.ExternalPreemptionContinuationError != Ia64ExternalFrameIdleContinuationError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionContinuationRejected;
        return false;
    }

    const Foundation::UInt64 idleDescriptorAddress = AddressOf(m_nativePreemptionIdleDescriptor);
    const Foundation::UInt64 idleEntryPoint = m_nativePreemptionIdleDescriptor[0U];
    const Foundation::UInt64 idleGlobalPointer = m_nativePreemptionIdleDescriptor[1U];
    const Ia64ExternalFrameIdleContinuationSnapshot idleSnapshot =
        m_idleContext.GetExternalFrameIdleContinuationSnapshot();
    if ((idleSnapshot.Configured != 1U) ||
        (idleSnapshot.MemoryStackVirtualStart != m_nativePreemptionStorage.CoordinatorMemoryStackAddress) ||
        (idleSnapshot.MemoryStackSizeBytes != m_nativePreemptionStorage.CoordinatorMemoryStackSizeBytes) ||
        (idleSnapshot.RseBackingStoreVirtualStart != m_nativePreemptionStorage.CoordinatorRseBackingStoreAddress) ||
        (idleSnapshot.RseBackingStoreSizeBytes != m_nativePreemptionStorage.CoordinatorRseBackingStoreSizeBytes) ||
        (idleSnapshot.EntryDescriptorAddress != idleDescriptorAddress) ||
        (idleSnapshot.EntryPointAddress != idleEntryPoint) || (idleSnapshot.EntryGlobalPointer != idleGlobalPointer) ||
        (idleSnapshot.ArgumentAddress != AddressOf(this)) || (idleSnapshot.PublishedGeneration != 0U))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionContinuationRejected;
        return false;
    }

    Interrupt::Ia64ExternalPreemptionFrame& frame = m_workerContext.GetExternalPreemptionFrame();
    const Foundation::UInt64 frameAddress = AddressOf(&frame);
    const Foundation::UInt64 controlAddress = AddressOf(m_nativePreemptionControl);
    const Interrupt::Ia64ExternalPreemptionCodeRange code = Interrupt::GetIa64ExternalPreemptionCodeRange();
    if ((code.Start == 0U) || (code.End <= code.Start) ||
        !m_runtimeMemory->HasLockedDataTranslationCoverage(controlAddress, sizeof(*m_nativePreemptionControl)) ||
        !m_runtimeMemory->HasLockedDataTranslationCoverage(frameAddress, sizeof(frame)) ||
        !m_runtimeMemory->HasLockedDataTranslationCoverage(workerMemoryStackStart, sizeof(m_workerMemoryStack)) ||
        !m_runtimeMemory->HasLockedDataTranslationCoverage(workerRseBackingStoreStart,
                                                           sizeof(m_workerRseBackingStore)) ||
        !m_runtimeMemory->HasLockedDataTranslationCoverage(m_nativePreemptionStorage.CoordinatorMemoryStackAddress,
                                                           m_nativePreemptionStorage.CoordinatorMemoryStackSizeBytes) ||
        !m_runtimeMemory->HasLockedDataTranslationCoverage(
            m_nativePreemptionStorage.CoordinatorRseBackingStoreAddress,
            m_nativePreemptionStorage.CoordinatorRseBackingStoreSizeBytes) ||
        !m_runtimeMemory->HasLockedInstructionTranslationCoverage(code.Start, code.End - code.Start))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionStorageRejected;
        return false;
    }

    m_failure.ExternalPreemptionControlError = Interrupt::ConfigureIa64ExternalPreemptionControl(
        *m_nativePreemptionControl,
        Interrupt::Ia64ExternalPreemptionControlConfiguration{
            .ProcessorIndex = m_root.Processor.GetValue(),
            .TimerVector = m_timerVector,
            .FrameAddress = frameAddress,
            .WorkerMemoryStackStart = workerMemoryStackStart,
            .WorkerMemoryStackEnd = workerMemoryStackEnd,
            .WorkerRseBackingStoreStart = workerRseBackingStoreStart,
            .WorkerRseBackingStoreEnd = workerRseBackingStoreEnd,
            .CoordinatorMemoryStackStart = m_nativePreemptionStorage.CoordinatorMemoryStackAddress,
            .CoordinatorMemoryStackEnd = coordinatorMemoryStackEnd,
            .CoordinatorRseBackingStoreStart = m_nativePreemptionStorage.CoordinatorRseBackingStoreAddress,
            .CoordinatorRseBackingStoreEnd = coordinatorRseBackingStoreEnd,
            .CoordinatorEntryPoint = m_nativePreemptionCoordinatorEntryPoint,
            .CoordinatorGlobalPointer = m_nativePreemptionCoordinatorGlobalPointer,
            .CoordinatorArgument = AddressOf(this),
            .SentinelEntryPoint = m_nativePreemptionSentinelEntryPoint,
            .SentinelEnd = m_nativePreemptionSentinelEnd,
        });
    if (m_failure.ExternalPreemptionControlError != Interrupt::Ia64ExternalPreemptionControlError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionControlRejected;
        return false;
    }

    m_failure.ExternalPreemptionStorageError =
        m_runtimeMemory->PublishExternalPreemptionControl(m_root.Processor, *m_nativePreemptionControl);
    if (m_failure.ExternalPreemptionStorageError != Memory::Ia64BspRuntimeExternalPreemptionError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionPublicationRejected;
        return false;
    }
    return true;
}
#endif

bool Ia64BspKernelRuntime::PrepareWorkerContext(
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    const WritableIa64KernelStorageView memoryStack{
        .Data = m_workerMemoryStack,
        .SizeBytes = sizeof(m_workerMemoryStack),
        .VirtualStart = AddressOf(m_workerMemoryStack),
    };
    const WritableIa64KernelStorageView rseBackingStore{
        .Data = m_workerRseBackingStore,
        .SizeBytes = sizeof(m_workerRseBackingStore),
        .VirtualStart = AddressOf(m_workerRseBackingStore),
    };
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64InitializeBspKernelWorkerDescriptor(m_workerEntryDescriptor);
    const auto workerEntry = reinterpret_cast<Ia64KernelContextEntry>(
        static_cast<Foundation::UIntPointer>(AddressOf(m_workerEntryDescriptor)));
#else
    const Ia64KernelContextEntry workerEntry{&WorkerEntry};
#endif
    if (!m_workerContext.AttachHighFloatingArea(m_workerHighFloating))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::WorkerContextRejected;
        return false;
    }
    m_failure.NativeContextError = m_workerContext.Prepare(implementedVirtualAddressMostSignificantBit, memoryStack,
                                                           rseBackingStore, workerEntry, this);
    if (m_failure.NativeContextError != Ia64KernelContextError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::WorkerContextRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::ValidateSoleResidentRoot() const noexcept
{
    if (!ValidateBorrowedInitialComponentRoot() || !m_userRoot.Processor.IsValid())
    {
        return false;
    }
    const Foundation::UInt8 processorBit =
        static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << m_userRoot.Processor.GetValue());
    const Kernel::Memory::AddressSpaceSnapshot addressSpace = m_userRoot.AddressSpaceStorage->GetSnapshot();
    const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot controller =
        m_userRoot.ControllerStorage->GetSnapshot();
    return (addressSpace.ResidentProcessorMask == processorBit) &&
           (controller.Processor.GetValue() == m_userRoot.Processor.GetValue()) &&
           (controller.ActiveStorage == m_userRoot.AddressSpaceStorage) &&
           controller.Active.IsEqualTo(m_userRoot.AddressSpaceIdentity);
}

bool Ia64BspKernelRuntime::ValidateUserProbePlacement(const Ia64UserExecutionProbe& userExecutionProbe) const noexcept
{
    if (m_root.AddressSpaceStorage == nullptr)
    {
        return false;
    }
    const Foundation::UInt64 start = AddressOf(&userExecutionProbe);
    Foundation::UInt64 end{0U};
    return TryGetEnd(start, sizeof(userExecutionProbe), end) && (end <= RegionZeroLimit) &&
           MappingContainsWritableRange(*m_root.AddressSpaceStorage, start, end);
}

bool Ia64BspKernelRuntime::ConstructTlbShootdownDispatcher() noexcept
{
    m_failure.LocalInvalidator = m_localTlbInvalidator.Initialize(m_palVirtualMemoryCapabilities);
    m_userProbeFailure.LocalInvalidator = m_failure.LocalInvalidator;
    if (m_failure.LocalInvalidator != Memory::Ia64LocalTlbInvalidatorInitializationError::None)
    {
        return false;
    }
    m_tlbShootdownDispatcher = ::new (static_cast<void*>(m_tlbShootdownDispatcherStorage))
        Kernel::Memory::TlbShootdownDispatcher{m_root.Processor, m_localTlbInvalidator};
#if defined(ASTERIA_TARGET_IA64)
    m_tlbShootdownDispatcher->UseCurrentProcessor();
#endif
    const Memory::Ia64LocalTlbInvalidatorSnapshot invalidator = m_localTlbInvalidator.GetSnapshot();
    return (invalidator.State == Memory::Ia64LocalTlbInvalidatorState::Ready) &&
           (invalidator.ImplementedVirtualAddressMostSignificantBit ==
            m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit) &&
           (m_tlbShootdownDispatcher->GetProcessor().GetValue() == m_root.Processor.GetValue());
}

bool Ia64BspKernelRuntime::PrepareUserProbeLaunchRecord(Ia64UserExecutionProbe& userExecutionProbe) noexcept
{
    m_userExecutionProbe = &userExecutionProbe;
    const Ia64UserModeVirtualRange supervisorMemoryStack{
        .VirtualStart = AddressOf(m_workerMemoryStack),
        .SizeBytes = sizeof(m_workerMemoryStack),
    };
    const Ia64UserModeVirtualRange supervisorRseBackingStore{
        .VirtualStart = AddressOf(m_workerRseBackingStore),
        .SizeBytes = sizeof(m_workerRseBackingStore),
    };
    const auto prepared = userExecutionProbe.PrepareLaunch(
        *m_runtimeMemory, m_objectRegistry, *m_issuer, m_resourceDomainIdentity, *m_resourceDomain, m_threadIdentity,
        *m_tlbShootdownDispatcher, supervisorMemoryStack, supervisorRseBackingStore,
        m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit);
    if (!prepared.HasValue())
    {
        m_userProbeFailure.Probe = prepared.GetErrorReference();
        return false;
    }

    m_userProbeLaunchRequest = prepared.GetValue();
    return (m_userExecutionProbe->GetState() == Ia64UserExecutionProbeState::Ready) &&
           (m_userProbeLaunchRequest.SupervisorContinuation != nullptr) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->MemoryStack.VirtualStart ==
            supervisorMemoryStack.VirtualStart) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->MemoryStack.SizeBytes ==
            supervisorMemoryStack.SizeBytes) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->RseBackingStore.VirtualStart ==
            supervisorRseBackingStore.VirtualStart) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->RseBackingStore.SizeBytes ==
            supervisorRseBackingStore.SizeBytes);
}

bool Ia64BspKernelRuntime::PrepareUserProbeCapability() noexcept
{
    if ((m_issuer == nullptr) || (m_resourceDomain == nullptr) || (m_capabilitySpace.GetActiveCount() != 0U))
    {
        m_userProbeFailure.Capability = Kernel::Object::CapabilityError::InvalidConfiguration;
        return false;
    }

    const auto revocationDomain = m_revocationRegistry.CreateRoot();
    if (!revocationDomain.HasValue())
    {
        m_userProbeFailure.Revocation = revocationDomain.GetError();
        return false;
    }
    m_userProbeRevocationDomain = revocationDomain.GetValue();

    constexpr Kernel::Object::CapabilityRights inspectRights{Kernel::Object::CapabilityRight::Inspect};
    const auto capability = m_issuer->Mint(m_capabilitySpace, m_protectionDomainIdentity.GetObjectId(), inspectRights,
                                           m_userProbeRevocationDomain, Kernel::Object::MonotonicTime::Infinite(), 0U);
    if (!capability.HasValue())
    {
        m_userProbeFailure.Capability = capability.GetError();
        return false;
    }
    m_userProbeCapability = capability.GetValue();

    const auto descriptor = m_capabilitySpace.Query(m_userProbeCapability);
    if (!descriptor.HasValue())
    {
        m_userProbeFailure.Capability = descriptor.GetError();
        return false;
    }
    const Kernel::Object::CapabilityDescriptor& value = descriptor.GetValue();
    if ((m_capabilitySpace.GetActiveCount() != 1U) || !m_userProbeCapability.IsValid() ||
        (m_userProbeCapability.GetSelector() != Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector) ||
        (value.Object.GetValue() != m_protectionDomainIdentity.GetObjectId().GetValue()) ||
        (value.Type != Kernel::Object::ObjectType::ProtectionDomain) || !value.Rights.IsEqualTo(inspectRights) ||
        (value.RevocationDomain.GetValue() != m_userProbeRevocationDomain.GetValue()) ||
        !value.LeaseDeadline.IsInfinite() || (value.SealedObjectSelector != 0U) ||
        (value.ChargeOwner.GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        m_userProbeFailure.Capability = Kernel::Object::CapabilityError::InvalidConfiguration;
        return false;
    }

    // The probe payload receives no DiagnosticChannel; r6/r7 stay zero.
    m_userProbeLaunchRequest.InitialArguments = Ia64UserModeInitialArguments{
        .Register4 = m_userProbeCapability.GetSelector(),
        .Register5 = m_userProbeCapability.GetAuthenticator(),
        .Register6 = 0U,
        .Register7 = 0U,
    };
    return true;
}

bool Ia64BspKernelRuntime::InitializeSystemCallWaitContinuation() noexcept
{
    if ((m_runtimeMemory == nullptr) || (m_userProbeLaunchRequest.SupervisorContinuation == nullptr) ||
        (m_systemCallEntryFrame != nullptr) || (m_systemCallBridgeContext.WaitContinuation != nullptr) ||
        (m_systemCallBridgeContext.WaitSetDispatcher != nullptr) || (m_waitSetSystemCallDispatcher == nullptr))
    {
        m_userProbeFailure.WaitContinuation = System::Ia64SystemCallWaitContinuationError::InvalidConfiguration;
        return false;
    }
    const Memory::Ia64BspRuntimeSystemCallPublicationSnapshot publication =
        m_runtimeMemory->GetSystemCallPublicationSnapshot();
    if (!publication.Published || (publication.LastError != Memory::Ia64BspRuntimeSystemCallPublicationError::None) ||
        (publication.Context.FrameAddress == 0U) ||
        (publication.Context.MemoryStackEnd <= publication.Context.MemoryStackStart) ||
        (publication.Context.RseBackingStoreEnd <= publication.Context.RseBackingStoreStart))
    {
        m_userProbeFailure.WaitContinuation = System::Ia64SystemCallWaitContinuationError::InvalidConfiguration;
        return false;
    }

    auto* const frame = reinterpret_cast<System::Ia64SystemCallEntryFrame*>(
        static_cast<Foundation::UIntPointer>(publication.Context.FrameAddress));
    const System::Ia64SystemCallWaitContinuationConfiguration configuration{
        .EntryFrame = frame,
        .SupervisorContinuation = m_userProbeLaunchRequest.SupervisorContinuation,
        .SystemCallMemoryStack =
            Ia64UserModeVirtualRange{
                .VirtualStart = publication.Context.MemoryStackStart,
                .SizeBytes = publication.Context.MemoryStackEnd - publication.Context.MemoryStackStart,
            },
        .SystemCallRseBackingStore =
            Ia64UserModeVirtualRange{
                .VirtualStart = publication.Context.RseBackingStoreStart,
                .SizeBytes = publication.Context.RseBackingStoreEnd - publication.Context.RseBackingStoreStart,
            },
        .Processor = m_root.Processor,
    };
    m_userProbeFailure.WaitContinuation = m_systemCallWaitContinuation.Initialize(configuration);
    if (m_userProbeFailure.WaitContinuation != System::Ia64SystemCallWaitContinuationError::None)
    {
        return false;
    }
    m_systemCallEntryFrame = frame;
    m_systemCallBridgeContext.WaitContinuation = &m_systemCallWaitContinuation;
    m_systemCallBridgeContext.WaitSetDispatcher = m_waitSetSystemCallDispatcher;
    return true;
}

bool Ia64BspKernelRuntime::InitializeUserProbeEndpoint() noexcept
{
    if ((m_issuer == nullptr) || (m_resourceDomain == nullptr) || (m_userProbeEndpoint != nullptr) ||
        m_userProbeEndpointIdentity.IsValid() || m_userProbeEndpointCapability.IsValid() ||
        !m_userProbeRevocationDomain.IsValid() || (m_capabilitySpace.GetActiveCount() != 1U))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointInitializationRejected;
        return false;
    }

    m_userProbeEndpoint = ::new (static_cast<void*>(m_userProbeEndpointStorage))
        Kernel::Ipc::Endpoint{m_capabilitySpace, m_resourceDomain->GetIdentifier(), UserProbeEndpointConfiguration};
    const Kernel::Ipc::EndpointQueueState initialQueue = m_userProbeEndpoint->GetQueueState();
    if (!m_userProbeEndpoint->IsReady() || !m_userProbeEndpoint->HasReceiverCapabilitySpace(m_capabilitySpace) ||
        (m_userProbeEndpoint->GetObjectType() != Kernel::Object::ObjectType::Endpoint) ||
        (m_userProbeEndpoint->GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        (initialQueue.QueuedBytes != 0U) || (initialQueue.NextSequence != 1U) || (initialQueue.QueuedMessages != 0U) ||
        (initialQueue.QueuedAttachments != 0U))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointInitializationRejected;
        return false;
    }

    const auto registered = m_issuer->CreateObject(*m_userProbeEndpoint);
    if (!registered.HasValue())
    {
        m_userProbeFailure.EndpointRegistry = registered.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointRegistrationRejected;
        return false;
    }
    m_userProbeEndpointIdentity = registered.GetValue();

    Kernel::Object::ObjectPin endpointPin;
    m_userProbeFailure.EndpointPin =
        m_objectRegistry.PinObject(m_userProbeEndpointIdentity, Kernel::Object::ObjectType::Endpoint, endpointPin);
    if (m_userProbeFailure.EndpointPin != Kernel::Object::ObjectPinError::Ok)
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointRegistrationRejected;
        return false;
    }
    const bool registeredEndpointMatches =
        endpointPin.IsValid() && (endpointPin.GetObjectId().GetValue() == m_userProbeEndpointIdentity.GetValue()) &&
        (endpointPin.GetObjectType() == Kernel::Object::ObjectType::Endpoint) &&
        (&endpointPin.GetObject() == m_userProbeEndpoint) &&
        (endpointPin.GetObject().GetChargeOwner().GetValue() == m_resourceDomain->GetIdentifier().GetValue());
    endpointPin.Reset();
    if (!registeredEndpointMatches)
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointRegistrationRejected;
        return false;
    }

    const auto capability = m_issuer->Mint(m_capabilitySpace, m_userProbeEndpointIdentity, UserProbeEndpointRights,
                                           m_userProbeRevocationDomain, Kernel::Object::MonotonicTime::Infinite(), 0U);
    if (!capability.HasValue())
    {
        m_userProbeFailure.Capability = capability.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointCapabilityRejected;
        return false;
    }
    m_userProbeEndpointCapability = capability.GetValue();

    auto invoked = m_capabilitySpace.Lookup(m_userProbeEndpointCapability, Kernel::Object::ObjectType::Endpoint,
                                            Kernel::Object::CapabilityOperation::Invoke);
    if (!invoked.HasValue())
    {
        m_userProbeFailure.Capability = invoked.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointCapabilityRejected;
        return false;
    }
    const Kernel::Object::CapabilityDescriptor& descriptor = invoked.GetValue().GetDescriptor();
    if ((m_capabilitySpace.GetActiveCount() != 2U) || !m_userProbeEndpointCapability.IsValid() ||
        (m_userProbeEndpointCapability.GetSelector() != Ia64UserExecutionProbeExpectedEndpointCapabilitySelector) ||
        (&invoked.GetValue().GetObject() != m_userProbeEndpoint) ||
        (descriptor.Object.GetValue() != m_userProbeEndpointIdentity.GetValue()) ||
        (descriptor.Type != Kernel::Object::ObjectType::Endpoint) ||
        !descriptor.Rights.IsEqualTo(UserProbeEndpointRights) ||
        (descriptor.RevocationDomain.GetValue() != m_userProbeRevocationDomain.GetValue()) ||
        !descriptor.LeaseDeadline.IsInfinite() || (descriptor.SealedObjectSelector != 0U) ||
        (descriptor.ChargeOwner.GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        m_userProbeFailure.Capability = Kernel::Object::CapabilityError::InvalidConfiguration;
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::EndpointCapabilityRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::PrepareUserProbeWaitSetCapability() noexcept
{
    if ((m_issuer == nullptr) || (m_resourceDomain == nullptr) || !m_userProbeWaitSetIdentity.IsValid() ||
        !m_userProbeRevocationDomain.IsValid() || m_userProbeWaitSetCapability.IsValid() ||
        (m_capabilitySpace.GetActiveCount() != 2U))
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSetCapabilityRejected;
        return false;
    }

    const auto capability =
        m_issuer->Mint(m_capabilitySpace, m_userProbeWaitSetIdentity.GetObjectId(), UserProbeWaitSetRights,
                       m_userProbeRevocationDomain, Kernel::Object::MonotonicTime::Infinite(), 0U);
    if (!capability.HasValue())
    {
        m_userProbeFailure.Capability = capability.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSetCapabilityRejected;
        return false;
    }
    m_userProbeWaitSetCapability = capability.GetValue();

    auto waited = m_capabilitySpace.Lookup(m_userProbeWaitSetCapability, Kernel::Object::ObjectType::WaitSet,
                                           Kernel::Object::CapabilityOperation::Wait);
    if (!waited.HasValue())
    {
        m_userProbeFailure.Capability = waited.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSetCapabilityRejected;
        return false;
    }
    const Kernel::Object::CapabilityDescriptor& descriptor = waited.GetValue().GetDescriptor();
    if ((m_capabilitySpace.GetActiveCount() != 3U) ||
        (m_userProbeWaitSetCapability.GetSelector() != Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector) ||
        (&waited.GetValue().GetObject() != &m_userProbeWaitSet) ||
        (descriptor.Object.GetValue() != m_userProbeWaitSetIdentity.GetValue()) ||
        (descriptor.Type != Kernel::Object::ObjectType::WaitSet) ||
        !descriptor.Rights.IsEqualTo(UserProbeWaitSetRights) ||
        (descriptor.RevocationDomain.GetValue() != m_userProbeRevocationDomain.GetValue()) ||
        !descriptor.LeaseDeadline.IsInfinite() || (descriptor.SealedObjectSelector != 0U) ||
        (descriptor.ChargeOwner.GetValue() != m_resourceDomain->GetIdentifier().GetValue()))
    {
        m_userProbeFailure.Capability = Kernel::Object::CapabilityError::InvalidConfiguration;
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSetCapabilityRejected;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::PrepareUserProbeBootstrap() noexcept
{
    m_userProbeBootstrapRecord = {};
    WriteLittleEndian64(m_userProbeBootstrapRecord.EndpointCapabilitySelector,
                        m_userProbeEndpointCapability.GetSelector());
    WriteLittleEndian64(m_userProbeBootstrapRecord.EndpointCapabilityAuthenticator,
                        m_userProbeEndpointCapability.GetAuthenticator());
    WriteLittleEndian64(m_userProbeBootstrapRecord.WaitSetCapabilitySelector,
                        m_userProbeWaitSetCapability.GetSelector());
    WriteLittleEndian64(m_userProbeBootstrapRecord.WaitSetCapabilityAuthenticator,
                        m_userProbeWaitSetCapability.GetAuthenticator());

    m_systemCallScratch.Clear();
    Kernel::Ipc::MessageEnvelope& message = m_systemCallScratch.GetMessage();
    message.Interface = Kernel::Ipc::InterfaceIdentifier{1U, 0U};
    message.TransactionIdentifier = 1U;
    message.Deadline = Kernel::Object::MonotonicTime::Infinite();
    message.MethodIdentifier = 1U;
    message.InterfaceVersionMajor = 1U;
    message.InterfaceVersionMinor = 0U;
    message.FixedPayloadSizeBytes = 0U;
    message.PayloadSizeBytes = 0U;
    message.Kind = Kernel::Ipc::MessageKind::Asynchronous;
    message.Cancellation = Kernel::Ipc::CancellationState::None;
    message.VariablePayloadCount = 0U;
    message.AttachmentCount = 0U;
    message.Reserved = 0U;
    message.ReservedAlignment = 0U;
    const auto encoded =
        Kernel::Ipc::MessageCodec::Encode(message, Kernel::Ipc::WritableByteView{
                                                       .Data = m_userProbeBootstrapRecord.Frame,
                                                       .SizeBytes = sizeof(m_userProbeBootstrapRecord.Frame),
                                                   });
    if (!encoded.HasValue())
    {
        m_userProbeFailure.BootstrapCodec = encoded.GetError();
        m_systemCallScratch.Clear();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::BootstrapEncodingRejected;
        return false;
    }
    const Foundation::Size encodedSize = encoded.GetValue();
    m_systemCallScratch.Clear();
    if ((encodedSize != sizeof(m_userProbeBootstrapRecord.Frame)) || !ValidateUserProbeBootstrapRecord())
    {
        if (encodedSize != sizeof(m_userProbeBootstrapRecord.Frame))
        {
            m_userProbeFailure.BootstrapCodec = Kernel::Ipc::MessageCodecError::SizeMismatch;
        }
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::BootstrapEncodingRejected;
        return false;
    }

    const auto destination = Kernel::Memory::UserMemoryRange::Create(Ia64UserExecutionProbeIpcBootstrapVirtualAddress,
                                                                     sizeof(m_userProbeBootstrapRecord));
    if (!destination.HasValue())
    {
        m_userProbeFailure.BootstrapRange = destination.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::BootstrapPublicationRejected;
        return false;
    }

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    const auto reservation = m_userMemory.PrepareCopyToUser(destination.GetValue());
    if (!reservation.HasValue())
    {
        m_userProbeFailure.BootstrapAccess = reservation.GetError();
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::BootstrapPublicationRejected;
        return false;
    }
    m_userProbeFailure.BootstrapAccess = m_userMemory.CommitCopyToUser(
        reservation.GetValue(), Kernel::Memory::ReadOnlyKernelBuffer{
                                    .Data = reinterpret_cast<const Foundation::UInt8*>(&m_userProbeBootstrapRecord),
                                    .SizeBytes = sizeof(m_userProbeBootstrapRecord),
                                });
    if (m_userProbeFailure.BootstrapAccess != Kernel::Memory::UserMemoryAccessError::None)
    {
        m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::BootstrapPublicationRejected;
        return false;
    }
#endif
    return true;
}

bool Ia64BspKernelRuntime::ValidateRetainedUserProbeLaunch() const noexcept
{
    const Ia64UserModeLaunchRequirements& requirements = m_userProbeLaunchRequest.Requirements;
    return (m_userExecutionProbe != nullptr) &&
           (m_userExecutionProbe->GetState() == Ia64UserExecutionProbeState::Ready) &&
           (m_userProbeLaunchRequest.Code.VirtualStart == Ia64UserExecutionProbeCodeVirtualStart) &&
           (m_userProbeLaunchRequest.Code.SizeBytes == Ia64UserExecutionProbeCodeSizeBytes) &&
           (m_userProbeLaunchRequest.MemoryStack.VirtualStart == Ia64UserExecutionProbeMemoryStackVirtualStart) &&
           (m_userProbeLaunchRequest.MemoryStack.SizeBytes == Ia64UserExecutionProbeMemoryStackSizeBytes) &&
           (m_userProbeLaunchRequest.RseBackingStore.VirtualStart ==
            Ia64UserExecutionProbeRseBackingStoreVirtualStart) &&
           (m_userProbeLaunchRequest.RseBackingStore.SizeBytes == Ia64UserExecutionProbeRseBackingStoreSizeBytes) &&
           (m_userProbeLaunchRequest.EntryBundleAddress == Ia64UserExecutionProbeCodeVirtualStart) &&
           (m_userProbeLaunchRequest.InitialGlobalPointer == 0U) &&
           (m_userProbeLaunchRequest.InitialArguments.Register4 == m_userProbeCapability.GetSelector()) &&
           (m_userProbeLaunchRequest.InitialArguments.Register5 == m_userProbeCapability.GetAuthenticator()) &&
           (m_userProbeLaunchRequest.InitialArguments.Register6 == 0U) &&
           (m_userProbeLaunchRequest.InitialArguments.Register7 == 0U) &&
           (requirements.CurrentProcessorStatusMask == Ia64UserModeCurrentProcessorStatusMask) &&
           (requirements.CurrentProcessorStatusValue == Ia64UserModeCurrentProcessorStatusValue) &&
           (requirements.UserProcessorStatus == Ia64UserModeInitialProcessorStatus) &&
           (requirements.ImplementedVirtualAddressMostSignificantBit ==
            m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit) &&
           (requirements.CurrentPrivilegeLevel == 0U) && (requirements.CurrentVirtualMachineMode == 0U) &&
           (requirements.CurrentInstructionSet == 0U) && (requirements.CurrentRegisterBank == 1U) &&
           (requirements.Reserved[0U] == 0U) && (requirements.Reserved[1U] == 0U) &&
           (requirements.Reserved[2U] == 0U) && (m_userProbeLaunchRequest.SupervisorContinuation != nullptr) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->MemoryStack.VirtualStart ==
            AddressOf(m_workerMemoryStack)) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->MemoryStack.SizeBytes == sizeof(m_workerMemoryStack)) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->RseBackingStore.VirtualStart ==
            AddressOf(m_workerRseBackingStore)) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->RseBackingStore.SizeBytes ==
            sizeof(m_workerRseBackingStore)) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->State == Ia64UserModeSupervisorContinuationState::Fresh) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->Owner == Ia64UserModeSupervisorContinuationOwner::None) &&
           (m_userProbeLaunchRequest.Reserved == 0U);
}

bool Ia64BspKernelRuntime::ValidateUserMemoryMappingLedger() const noexcept
{
    if (!ValidateBorrowedInitialComponentRoot() || (m_userRoot.AddressSpaceStorage == nullptr))
    {
        return false;
    }
    const Kernel::Protection::ProtectionDomainSnapshot protection = m_protectionDomain.GetSnapshot();
    const auto protectionBinding = m_protectionDomain.GetBinding();
    if ((protection.State != Kernel::Protection::ProtectionDomainState::Ready) ||
        !protection.Identity.IsEqualTo(m_protectionDomainIdentity) ||
        !protection.AddressSpace.IsEqualTo(m_userRoot.AddressSpaceIdentity) || !protectionBinding.HasValue() ||
        !protectionBinding.GetValue().Identity.IsEqualTo(m_protectionDomainIdentity) ||
        !protectionBinding.GetValue().AddressSpace.IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        (protectionBinding.GetValue().AddressSpaceStorage != m_userRoot.AddressSpaceStorage) ||
        (protectionBinding.GetValue().Capabilities != &m_capabilitySpace))
    {
        return false;
    }

    const Kernel::Memory::AddressSpaceSnapshot snapshot = m_userRoot.AddressSpaceStorage->GetSnapshot();
    if ((snapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !snapshot.Identity.IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        (snapshot.MappingCount > Kernel::Memory::MaximumAddressSpaceMappings))
    {
        return false;
    }

    Foundation::Size matchCount{0U};
    for (Foundation::Size index{0U}; index < snapshot.MappingCount; ++index)
    {
        const auto mapping = m_userRoot.AddressSpaceStorage->TryGetMapping(index);
        if (!mapping.HasValue())
        {
            return false;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& record = mapping.GetValue();
        if (record.VirtualStart.GetValue() != Ia64UserExecutionProbeMemoryStackVirtualStart)
        {
            continue;
        }
        if (!record.MemoryObject.IsValid() || record.DeviceRegion.IsValid() || !record.Frame.IsValid() ||
            (record.Pages.GetValue() != Ia64UserExecutionProbeMemoryStackPageCount) ||
            (record.MemoryObjectOffsetPages != 0U) ||
            (record.Permissions.GetBits() != UserStackPermissions.GetBits()) ||
            (record.CachePolicy != Kernel::Memory::MemoryCachePolicy::WriteBack) ||
            (record.Kind != Kernel::Memory::FrameKind::Ram) ||
            (record.Commitment != Kernel::Memory::MemoryCommitment::Pinned) || !record.RuntimeMapping ||
            (record.Reserved[0U] != 0U) || (record.Reserved[1U] != 0U) || (record.Reserved[2U] != 0U))
        {
            return false;
        }
        ++matchCount;
    }
    return matchCount == 1U;
}

bool Ia64BspKernelRuntime::InitializeUserMemory() noexcept
{
    if (m_userMemory.IsReady() || m_userMemory.GetAddressSpaceId().IsValid() || !ValidateRetainedUserProbeLaunch() ||
        !ValidateUserMemoryMappingLedger())
    {
        m_userProbeFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return false;
    }

    const Kernel::Memory::DirectUserMemoryMapping mapping{
        .UserStart = Kernel::Memory::VirtualAddress{Ia64UserExecutionProbeMemoryStackVirtualStart},
        .DirectStart = reinterpret_cast<Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(Ia64UserExecutionProbeMemoryStackVirtualStart)),
        .Pages = Kernel::Memory::PageCount{Ia64UserExecutionProbeMemoryStackPageCount},
        .Permissions = UserStackPermissions,
        .Kind = Kernel::Memory::FrameKind::Ram,
    };
    m_userProbeFailure.UserMemory =
        m_userMemory.Initialize(m_userRoot.AddressSpaceIdentity, &mapping, 1U, m_userRoot.AddressSpaceStorage);
    if (m_userProbeFailure.UserMemory != Kernel::Memory::DirectUserMemoryAccessError::None)
    {
        return false;
    }
    if (!ValidateUserMemoryPostconditions())
    {
        m_userProbeFailure.UserMemory = Kernel::Memory::DirectUserMemoryAccessError::InvalidMapping;
        return false;
    }
    return true;
}

bool Ia64BspKernelRuntime::ValidateUserMemoryPostconditions() const noexcept
{
    if ((m_runtimeMemory == nullptr) || (m_runLoop == nullptr) || !m_userMemory.IsReady() ||
        !m_userMemory.GetAddressSpaceId().IsEqualTo(m_userRoot.AddressSpaceIdentity) ||
        (m_systemCallBridgeContext.RunLoop != m_runLoop) || (m_systemCallBridgeContext.UserMemory != &m_userMemory) ||
        (m_systemCallBridgeContext.Scratch != &m_systemCallScratch) ||
        (m_systemCallBridgeContext.WaitContinuation != &m_systemCallWaitContinuation) ||
        (m_systemCallBridgeContext.WaitSetDispatcher != m_waitSetSystemCallDispatcher) ||
        !ValidateRetainedUserProbeLaunch() || !ValidateUserMemoryMappingLedger())
    {
        return false;
    }
    const Memory::Ia64BspRuntimeSystemCallPublicationSnapshot publication =
        m_runtimeMemory->GetSystemCallPublicationSnapshot();
    return publication.Published && (publication.LastError == Memory::Ia64BspRuntimeSystemCallPublicationError::None) &&
           (publication.Context.BridgeContextAddress == AddressOf(&m_systemCallBridgeContext)) &&
           (publication.Context.ProcessorIndex == m_root.Processor.GetValue());
}

bool Ia64BspKernelRuntime::ValidateUserProbeBootstrapRecord() const noexcept
{
    return m_userProbeEndpointCapability.IsValid() && m_userProbeWaitSetCapability.IsValid() &&
           IsLittleEndian64(m_userProbeBootstrapRecord.EndpointCapabilitySelector,
                            m_userProbeEndpointCapability.GetSelector()) &&
           IsLittleEndian64(m_userProbeBootstrapRecord.EndpointCapabilityAuthenticator,
                            m_userProbeEndpointCapability.GetAuthenticator()) &&
           IsLittleEndian64(m_userProbeBootstrapRecord.WaitSetCapabilitySelector,
                            m_userProbeWaitSetCapability.GetSelector()) &&
           IsLittleEndian64(m_userProbeBootstrapRecord.WaitSetCapabilityAuthenticator,
                            m_userProbeWaitSetCapability.GetAuthenticator()) &&
           BytesAreEqual(m_userProbeBootstrapRecord.Frame, UserProbeCanonicalFrame, sizeof(UserProbeCanonicalFrame));
}

bool Ia64BspKernelRuntime::ValidateUserProbeIpcPostconditions() noexcept
{
    if ((m_resourceDomain == nullptr) || (m_userProbeEndpoint == nullptr) || !m_userProbeEndpointIdentity.IsValid() ||
        !m_userProbeEndpointCapability.IsValid() || !m_userProbeWaitSetIdentity.IsValid() ||
        !m_userProbeWaitSetCapability.IsValid() ||
        (m_userProbeCapability.GetSelector() != Ia64UserExecutionProbeExpectedProtectionDomainCapabilitySelector) ||
        (m_userProbeEndpointCapability.GetSelector() != Ia64UserExecutionProbeExpectedEndpointCapabilitySelector) ||
        (m_userProbeWaitSetCapability.GetSelector() != Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector) ||
        !m_userProbeRevocationDomain.IsValid() || (m_capabilitySpace.GetActiveCount() != 3U) ||
        !m_userProbeEndpoint->IsReady() || !m_userProbeEndpoint->HasReceiverCapabilitySpace(m_capabilitySpace) ||
        (m_userProbeEndpoint->GetObjectType() != Kernel::Object::ObjectType::Endpoint) ||
        (m_userProbeEndpoint->GetChargeOwner().GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        !m_userProbeWaitSet.IsInitialized() || !ValidateUserMemoryPostconditions() ||
        !ValidateUserProbeBootstrapRecord())
    {
        return false;
    }

    const Kernel::Ipc::EndpointQueueState queue = m_userProbeEndpoint->GetQueueState();
    if ((queue.QueuedBytes != 0U) || (queue.NextSequence != 1U) || (queue.QueuedMessages != 0U) ||
        (queue.QueuedAttachments != 0U) || (queue.Reserved[0U] != 0U) || (queue.Reserved[1U] != 0U) ||
        (queue.Reserved[2U] != 0U) || (queue.Reserved[3U] != 0U) || (queue.Reserved[4U] != 0U) ||
        (queue.Reserved[5U] != 0U))
    {
        return false;
    }

    Kernel::Object::ObjectPin endpointPin;
    m_userProbeFailure.EndpointPin =
        m_objectRegistry.PinObject(m_userProbeEndpointIdentity, Kernel::Object::ObjectType::Endpoint, endpointPin);
    if (m_userProbeFailure.EndpointPin != Kernel::Object::ObjectPinError::Ok)
    {
        return false;
    }
    const bool registrationMatches = endpointPin.IsValid() &&
                                     (endpointPin.GetObjectId().GetValue() == m_userProbeEndpointIdentity.GetValue()) &&
                                     (&endpointPin.GetObject() == m_userProbeEndpoint);
    endpointPin.Reset();
    if (!registrationMatches)
    {
        return false;
    }

    auto invoked = m_capabilitySpace.Lookup(m_userProbeEndpointCapability, Kernel::Object::ObjectType::Endpoint,
                                            Kernel::Object::CapabilityOperation::Invoke);
    if (!invoked.HasValue())
    {
        m_userProbeFailure.Capability = invoked.GetError();
        return false;
    }
    const Kernel::Object::CapabilityDescriptor& descriptor = invoked.GetValue().GetDescriptor();
    auto waited = m_capabilitySpace.Lookup(m_userProbeWaitSetCapability, Kernel::Object::ObjectType::WaitSet,
                                           Kernel::Object::CapabilityOperation::Wait);
    if (!waited.HasValue())
    {
        m_userProbeFailure.Capability = waited.GetError();
        return false;
    }
    const Kernel::Object::CapabilityDescriptor& waitDescriptor = waited.GetValue().GetDescriptor();
    return (&invoked.GetValue().GetObject() == m_userProbeEndpoint) &&
           (descriptor.Object.GetValue() == m_userProbeEndpointIdentity.GetValue()) &&
           (descriptor.Type == Kernel::Object::ObjectType::Endpoint) &&
           descriptor.Rights.IsEqualTo(UserProbeEndpointRights) &&
           (descriptor.RevocationDomain.GetValue() == m_userProbeRevocationDomain.GetValue()) &&
           descriptor.RevocationDomain.IsValid() && descriptor.LeaseDeadline.IsInfinite() &&
           (descriptor.SealedObjectSelector == 0U) &&
           (descriptor.ChargeOwner.GetValue() == m_resourceDomain->GetIdentifier().GetValue()) &&
           (&waited.GetValue().GetObject() == &m_userProbeWaitSet) &&
           (waitDescriptor.Object.GetValue() == m_userProbeWaitSetIdentity.GetValue()) &&
           (waitDescriptor.Type == Kernel::Object::ObjectType::WaitSet) &&
           waitDescriptor.Rights.IsEqualTo(UserProbeWaitSetRights) &&
           (waitDescriptor.RevocationDomain.GetValue() == m_userProbeRevocationDomain.GetValue()) &&
           waitDescriptor.LeaseDeadline.IsInfinite() && (waitDescriptor.SealedObjectSelector == 0U) &&
           (waitDescriptor.ChargeOwner.GetValue() == m_resourceDomain->GetIdentifier().GetValue());
}

void Ia64BspKernelRuntime::ConstructControllerAndRunLoop() noexcept
{
    m_controller = ::new (static_cast<void*>(m_controllerStorage))
        Kernel::Scheduler::ProcessorRunController{m_objectRegistry, m_timerQueue, m_localTimer, m_root.Processor,
                                                  Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline};
    m_runLoop = ::new (static_cast<void*>(m_runLoopStorage))
        Ia64ProcessorRunLoop{*m_controller, *m_root.ControllerStorage, m_idleContext};
    m_homeWakes[0U] = &m_controller->GetWakeFacade();
    m_homeWakes[0U]->BindHomes(m_homeWakes);
    m_waitSetThreadDispatcher = ::new (static_cast<void*>(m_waitSetThreadDispatcherStorage))
        Kernel::Ipc::WaitSetThreadDispatcher{m_objectRegistry, m_controller->GetWakeFacade()};
    m_waitSetSystemCallDispatcher = ::new (static_cast<void*>(m_waitSetSystemCallDispatcherStorage))
        Kernel::System::WaitSetSystemCallDispatcher{m_objectRegistry, *m_waitSetThreadDispatcher};
    m_systemCallBridgeContext = System::Ia64SystemCallBridgeContext{
        .RunLoop = m_runLoop,
        .UserMemory = &m_userMemory,
        .Scratch = &m_systemCallScratch,
        .WaitContinuation = nullptr,
        .WaitSetDispatcher = nullptr,
        .Thread = nullptr,
        .PagerResolution = nullptr,
        .ObjectManagement = nullptr,
    };
}

Controller& Ia64BspKernelRuntime::Controller() noexcept
{
    return *m_controller;
}

const Controller& Ia64BspKernelRuntime::Controller() const noexcept
{
    return *m_controller;
}

Ia64ProcessorRunLoop& Ia64BspKernelRuntime::RunLoop() noexcept
{
    return *m_runLoop;
}

const Ia64ProcessorRunLoop& Ia64BspKernelRuntime::RunLoop() const noexcept
{
    return *m_runLoop;
}

bool Ia64BspKernelRuntime::ValidatePreparationPostconditions() const noexcept
{
    if ((m_controller == nullptr) || (m_runLoop == nullptr) || (m_waitSetThreadDispatcher == nullptr) ||
        (m_waitSetSystemCallDispatcher == nullptr) || (m_tlbShootdownDispatcher == nullptr) ||
        (m_runtimeMemory == nullptr) || (m_systemCallBridgeContext.RunLoop != m_runLoop) ||
        (m_systemCallBridgeContext.UserMemory != &m_userMemory) ||
        (m_systemCallBridgeContext.Scratch != &m_systemCallScratch) ||
        (m_systemCallBridgeContext.WaitContinuation != nullptr) ||
        (m_systemCallBridgeContext.WaitSetDispatcher != nullptr) || m_userMemory.IsReady() ||
        m_userMemory.GetAddressSpaceId().IsValid() || !ValidateBorrowedRoot() ||
        !ValidateBorrowedInitialComponentRoot())
    {
        return false;
    }
    const Memory::Ia64LocalTlbInvalidatorSnapshot invalidator = m_localTlbInvalidator.GetSnapshot();
    const Memory::Ia64BspRuntimeSystemCallPublicationSnapshot systemCallPublication =
        m_runtimeMemory->GetSystemCallPublicationSnapshot();
    return (invalidator.State == Memory::Ia64LocalTlbInvalidatorState::Ready) &&
           (invalidator.ImplementedVirtualAddressMostSignificantBit ==
            m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit) &&
           (m_tlbShootdownDispatcher->GetProcessor().GetValue() == m_root.Processor.GetValue()) &&
           systemCallPublication.Published &&
           (systemCallPublication.LastError == Memory::Ia64BspRuntimeSystemCallPublicationError::None) &&
           (systemCallPublication.LastContextValidationError == System::Ia64SystemCallContextValidationError::None) &&
           (systemCallPublication.LastInterruptVectorTableError ==
            Memory::Ia64RuntimeInterruptVectorTablePublicationError::None) &&
           (systemCallPublication.Context.BridgeContextAddress == AddressOf(&m_systemCallBridgeContext)) &&
           (systemCallPublication.Context.ProcessorIndex == m_root.Processor.GetValue()) &&
           ValidateLocalTimerProbeObjects() && ValidatePreparedObjects() && ValidatePreparedDispatchers();
}

bool Ia64BspKernelRuntime::ValidatePreparedObjects() const noexcept
{
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    const Kernel::Time::TimerSnapshot probeTimer = m_localTimerProbeTimer.GetSnapshot();
    const Kernel::Ipc::NotificationSnapshot notification = m_localTimerProbeNotification.GetSnapshot();
    const Kernel::Ipc::WaitSetSnapshot waitSet = m_userProbeWaitSet.GetSnapshot();
    const Kernel::Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    const Kernel::Protection::ProtectionDomainSnapshot protection = m_protectionDomain.GetSnapshot();
    const auto protectionBinding = m_protectionDomain.GetBinding();
    const Kernel::Object::CapabilitySpaceIdentifier capabilitySpaceIdentity = m_capabilitySpace.GetIdentifier();
    const Kernel::Scheduler::ThreadSnapshot thread = m_thread.GetSnapshot();
    const Kernel::Scheduler::SchedulingContextSnapshot scheduling = m_schedulingContext.GetSnapshot();
    return (timer.State == Time::Ia64LocalTimerState::Ready) && (timer.Vector == m_timerVector) &&
           (timer.Armed == 0U) && (timer.ProgrammedDeadlineTicks == 0U) &&
           (probeTimer.State == Kernel::Time::TimerState::Disarmed) && !probeTimer.Token.IsValid() &&
           (probeTimer.DeadlineTicks == 0U) && (notification.State == Kernel::Ipc::NotificationState::Ready) &&
           (notification.Pending == 0U) && (notification.LastClaimSequence == 0U) &&
           m_userProbeWaitSetIdentity.IsValid() && m_userProbeWaitSetIdentity.IsFromRegistry(m_objectRegistry) &&
           (waitSet.State == Kernel::Ipc::WaitSetState::Ready) && (waitSet.RegisteredEntries == 0U) &&
           (waitSet.ClaimedEntries == 0U) && (waitSet.LastRegistrationSequence == 0U) &&
           (waitSet.LastClaimSequence == 0U) && (timerQueue.NextDeadlineTicks == 0U) &&
           (timerQueue.NextClaimSequence == 1U) && (timerQueue.ScheduledEntries == 0U) &&
           (timerQueue.PendingEntries == 0U) && (timerQueue.ClaimedEntries == 0U) && m_revocationRegistry.IsReady() &&
           m_capabilitySpace.IsReady() && m_capabilitySpace.IsFromRegistry(m_objectRegistry) &&
           (m_capabilitySpace.GetActiveCount() == 0U) && capabilitySpaceIdentity.IsValid() &&
           (protection.Kind == Kernel::Protection::ProtectionDomainKind::UserCapable) &&
           (protection.State == Kernel::Protection::ProtectionDomainState::Ready) &&
           protection.Identity.IsEqualTo(m_protectionDomainIdentity) &&
           protection.AddressSpace.IsEqualTo(m_userRoot.AddressSpaceIdentity) &&
           (protection.CapabilitySpace.GetValue() == capabilitySpaceIdentity.GetValue()) &&
           protectionBinding.HasValue() &&
           protectionBinding.GetValue().AddressSpace.IsEqualTo(m_userRoot.AddressSpaceIdentity) &&
           (protectionBinding.GetValue().AddressSpaceStorage == m_userRoot.AddressSpaceStorage) &&
           (protectionBinding.GetValue().Capabilities == &m_capabilitySpace) &&
           (protectionBinding.GetValue().CapabilitySpace.GetValue() == capabilitySpaceIdentity.GetValue()) &&
           (thread.State == Kernel::Scheduler::ThreadState::Dormant) &&
           (scheduling.State == Kernel::Scheduler::SchedulingContextState::Bound) &&
           scheduling.BaseThread.IsEqualTo(m_threadIdentity) && scheduling.CurrentThread.IsEqualTo(m_threadIdentity) &&
           (m_idleContext.GetSnapshot().State == Ia64KernelContextState::Fresh) && m_workerContext.IsPrepared();
}

bool Ia64BspKernelRuntime::ValidatePreparedDispatchers() const noexcept
{
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = RunLoop().GetSnapshot();
    return (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Mode == Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (runLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) && (runLoop.BindingCount == 0U) &&
           (runLoop.ActiveThread == nullptr) && (runLoop.ActiveAddressSpace == nullptr);
}

PreparationResult Ia64BspKernelRuntime::Prepare(
    Memory::Ia64BspRuntimeMemory& runtimeMemory, const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, Kernel::Object::RootCapabilityIssuer& issuer,
    const Memory::PalVirtualMemoryCapabilities& palVirtualMemoryCapabilities,
    const Foundation::UInt8 timerVector) noexcept
{
    if ((m_state == Ia64BspKernelRuntimeState::Prepared) || (m_state == Ia64BspKernelRuntimeState::Dispatching) ||
        (m_state == Ia64BspKernelRuntimeState::Complete))
    {
        return PreparationResult::Failure(Ia64BspKernelRuntimeError::AlreadyPrepared);
    }
    if (m_state == Ia64BspKernelRuntimeState::Failed)
    {
        return PreparationResult::Failure(Ia64BspKernelRuntimeError::PreviousPreparationFailed);
    }

    const auto borrowed = runtimeMemory.BorrowReadyRoot();
    if (!borrowed.HasValue())
    {
        m_failure.RootBorrowError = borrowed.GetError();
        return FailPreparation(Ia64BspKernelRuntimeError::RuntimeRootBorrowRejected);
    }
    m_root = borrowed.GetValue();
    if (!ValidateBorrowedRoot())
    {
        return FailPreparation(Ia64BspKernelRuntimeError::RuntimeRootRejected);
    }
    const auto initialComponentRoot = runtimeMemory.BorrowReadyInitialComponentRoot();
    if (!initialComponentRoot.HasValue())
    {
        m_failure.RootBorrowError = initialComponentRoot.GetError();
        return FailPreparation(Ia64BspKernelRuntimeError::InitialComponentRootBorrowRejected);
    }
    m_userRoot = initialComponentRoot.GetValue();
    if (!ValidateBorrowedInitialComponentRoot())
    {
        return FailPreparation(Ia64BspKernelRuntimeError::InitialComponentRootRejected);
    }
    if (!ValidatePlacementAndMapping())
    {
        return FailPreparation(Ia64BspKernelRuntimeError::InvalidPlacement);
    }
    if (!ValidateResourceDomain(resourceDomainIdentity, resourceDomain))
    {
        return FailPreparation(Ia64BspKernelRuntimeError::ResourceDomainRejected);
    }

    m_runtimeMemory = &runtimeMemory;
    m_resourceDomainIdentity = resourceDomainIdentity;
    m_resourceDomain = &resourceDomain;
    m_issuer = &issuer;
    m_palVirtualMemoryCapabilities = palVirtualMemoryCapabilities;

    if (!ConstructTlbShootdownDispatcher())
    {
        return FailPreparation(Ia64BspKernelRuntimeError::LocalTlbInvalidatorRejected);
    }

    m_timerVector = timerVector;
    m_failure.LocalTimerError = m_localTimer.Initialize(timerVector);
    if (m_failure.LocalTimerError != Time::Ia64LocalTimerInitializationError::None)
    {
        return FailPreparation(Ia64BspKernelRuntimeError::LocalTimerRejected);
    }
    const Kernel::Object::ResourceDomainId chargeOwner = resourceDomain.GetIdentifier();
    if (!InitializeLocalTimerProbeObjects(chargeOwner, issuer))
    {
        return FailPreparation(m_failure.Error);
    }
    if (!InitializeUserProbeWaitSet(chargeOwner, issuer))
    {
        return FailPreparation(m_failure.Error);
    }
    if (!InitializeProtectionDomain(resourceDomainIdentity, resourceDomain, issuer))
    {
        return FailPreparation(m_failure.Error);
    }

    if (!InitializeThread(chargeOwner, issuer))
    {
        return FailPreparation(m_failure.Error);
    }
    if (!InitializeSchedulingContext(chargeOwner, issuer))
    {
        return FailPreparation(m_failure.Error);
    }
    if (!PrepareWorkerContext(m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit))
    {
        return FailPreparation(m_failure.Error);
    }

    ConstructControllerAndRunLoop();
    m_failure.SystemCallPublicationError =
        runtimeMemory.PublishSystemCallEntryContext(m_root.Processor, AddressOf(&m_systemCallBridgeContext));
    if (m_failure.SystemCallPublicationError != Memory::Ia64BspRuntimeSystemCallPublicationError::None)
    {
        return FailPreparation(Ia64BspKernelRuntimeError::SystemCallPublicationRejected);
    }
    if (!ValidatePreparationPostconditions())
    {
        return FailPreparation(Ia64BspKernelRuntimeError::PreparationPostconditionRejected);
    }

    m_state = Ia64BspKernelRuntimeState::Prepared;
    return PreparationResult::Success(Ia64BspKernelRuntimePreparationSummary{
        .AddressSpaceIdentity = m_userRoot.AddressSpaceIdentity,
        .ProtectionDomainIdentity = m_protectionDomainIdentity,
        .ThreadIdentity = m_threadIdentity,
        .SchedulingContextIdentity = m_schedulingContextIdentity,
        .Processor = m_root.Processor,
        .TimerVector = m_timerVector,
        .Reserved = {},
    });
}

Ia64BspSharedRootShootdownError
Ia64BspKernelRuntime::PreflightSharedRootShootdown(const Kernel::Memory::PhysicalPageRange& physicalPage,
                                                   const Foundation::UInt8 profiledProcessorMask) noexcept
{
    constexpr Foundation::UInt8 validProcessorMask{
        static_cast<Foundation::UInt8>((Foundation::UInt8{1U} << Kernel::Scheduler::MaximumProcessorCount) - 1U)};
    if ((m_state != Ia64BspKernelRuntimeState::Prepared) || (m_runtimeMemory == nullptr) ||
        (m_resourceDomain == nullptr) || (m_issuer == nullptr) || (m_tlbShootdownDispatcher == nullptr))
    {
        return Ia64BspSharedRootShootdownError::RuntimeNotPrepared;
    }
    if (!m_issuer->IsFromRegistry(m_objectRegistry) || !m_root.Processor.IsValid() ||
        (m_root.Processor.GetValue() != 0U) || !m_tlbShootdownDispatcher->GetProcessor().IsValid() ||
        (m_tlbShootdownDispatcher->GetProcessor().GetValue() != 0U))
    {
        return Ia64BspSharedRootShootdownError::InvalidDependencies;
    }
    if ((profiledProcessorMask == 0U) || ((profiledProcessorMask & 0x01U) == 0U) ||
        ((profiledProcessorMask & static_cast<Foundation::UInt8>(~validProcessorMask)) != 0U))
    {
        return Ia64BspSharedRootShootdownError::InvalidProcessorProfile;
    }

    Foundation::UInt64 physicalEnd{0U};
    const Foundation::UInt8 physicalAddressBits = m_palVirtualMemoryCapabilities.Translation.PhysicalAddressBits;
    if ((physicalPage.PhysicalStart == 0U) || ((physicalPage.PhysicalStart & PageOffsetMask) != 0U) ||
        (physicalPage.PageCount != 1U) || ((physicalPage.Attributes & FirmwareWriteBackAttribute) == 0U) ||
        ((physicalPage.Attributes & FirmwareRuntimeAttribute) != 0U) ||
        (physicalPage.FirmwareType != ConventionalFirmwareMemoryType) || (physicalPage.Reserved != 0U) ||
        (physicalAddressBits < 32U) || (physicalAddressBits > 63U) ||
        !TryGetPageRangeEnd(physicalPage.PhysicalStart, physicalPage.PageCount, physicalEnd) ||
        (physicalEnd > (Foundation::UInt64{1U} << physicalAddressBits)))
    {
        return Ia64BspSharedRootShootdownError::InvalidPhysicalPage;
    }
    if (!ValidateBorrowedRoot())
    {
        return Ia64BspSharedRootShootdownError::RootRejected;
    }

    m_sharedRootShootdownInitialRoot = m_root.AddressSpaceStorage->GetSnapshot();
    m_sharedRootShootdownObservedRoot = m_sharedRootShootdownInitialRoot;
    if ((m_sharedRootShootdownInitialRoot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !m_sharedRootShootdownInitialRoot.Identity.IsEqualTo(m_root.AddressSpaceIdentity) ||
        (m_sharedRootShootdownInitialRoot.ResidentProcessorMask != profiledProcessorMask) ||
        (m_sharedRootShootdownInitialRoot.ShootdownSequence != 0U) ||
        (m_sharedRootShootdownInitialRoot.TransitionSequence != 0U) ||
        (m_sharedRootShootdownInitialRoot.TargetProcessorMask != 0U) ||
        (m_sharedRootShootdownInitialRoot.AcknowledgedProcessorMask != 0U) ||
        !BytesAreZero(m_sharedRootShootdownInitialRoot.Reserved, sizeof(m_sharedRootShootdownInitialRoot.Reserved)))
    {
        return Ia64BspSharedRootShootdownError::RootRejected;
    }
    if (m_sharedRootShootdownInitialRoot.MappingCount >= Kernel::Memory::MaximumAddressSpaceMappings)
    {
        return Ia64BspSharedRootShootdownError::MappingCapacityExceeded;
    }

    constexpr Foundation::UInt64 virtualEnd{Ia64BspSharedRootShootdownVirtualStart +
                                            Kernel::Memory::MappingPageSizeBytes};
    for (Foundation::Size index{0U}; index < m_sharedRootShootdownInitialRoot.MappingCount; ++index)
    {
        const auto existingResult = m_root.AddressSpaceStorage->TryGetMapping(index);
        if (!existingResult.HasValue())
        {
            return Ia64BspSharedRootShootdownError::RootRejected;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& existing = existingResult.GetValue();
        Foundation::UInt64 existingVirtualEnd{0U};
        Foundation::UInt64 existingPhysicalEnd{0U};
        if (!TryGetPageRangeEnd(existing.VirtualStart.GetValue(), existing.Pages.GetValue(), existingVirtualEnd) ||
            !TryGetPageRangeEnd(existing.PhysicalStart.GetValue(), existing.Pages.GetValue(), existingPhysicalEnd))
        {
            return Ia64BspSharedRootShootdownError::RootRejected;
        }
        if (RangesOverlap(Ia64BspSharedRootShootdownVirtualStart, virtualEnd, existing.VirtualStart.GetValue(),
                          existingVirtualEnd))
        {
            return Ia64BspSharedRootShootdownError::VirtualRangeOccupied;
        }
        if (RangesOverlap(physicalPage.PhysicalStart, physicalEnd, existing.PhysicalStart.GetValue(),
                          existingPhysicalEnd))
        {
            return Ia64BspSharedRootShootdownError::PhysicalStorageAlreadyMapped;
        }
    }

    m_sharedRootShootdownInitialRegistryCount = m_objectRegistry.GetActiveCount();
    const Foundation::Size registryCapacity = m_objectRegistry.GetCapacity();
    if ((m_sharedRootShootdownInitialRegistryCount > registryCapacity) ||
        (2U > (registryCapacity - m_sharedRootShootdownInitialRegistryCount)))
    {
        return Ia64BspSharedRootShootdownError::RegistryCapacityExceeded;
    }
    m_sharedRootShootdownInitialResources = m_resourceDomain->GetSnapshot();
    m_sharedRootShootdownFinalResources = m_sharedRootShootdownInitialResources;
    if ((m_sharedRootShootdownInitialResources.State != Kernel::Resource::ResourceDomainState::Ready) ||
        !m_resourceDomainIdentity.IsValid() || !m_resourceDomainIdentity.IsFromRegistry(m_objectRegistry) ||
        (m_sharedRootShootdownInitialResources.Identity.GetValue() != m_resourceDomain->GetIdentifier().GetValue()) ||
        !ResourceCapacityIsSufficient(m_sharedRootShootdownInitialResources))
    {
        return Ia64BspSharedRootShootdownError::ResourceCapacityExceeded;
    }

    m_sharedRootShootdownSummary.AddressSpaceIdentity = m_root.AddressSpaceIdentity;
    m_sharedRootShootdownSummary.TargetProcessorMask = profiledProcessorMask;
    return Ia64BspSharedRootShootdownError::None;
}

Ia64BspSharedRootShootdownError Ia64BspKernelRuntime::InitializeSharedRootShootdownObjects(
    const Kernel::Memory::PhysicalPageRange& physicalPage) noexcept
{
    const Kernel::Object::ResourceDomainId chargeOwner = m_resourceDomain->GetIdentifier();
    m_sharedRootShootdownFailure.Frame = m_sharedRootShootdownFrame.Initialize(
        chargeOwner, Kernel::Memory::FrameDescriptor{
                         .PhysicalStart = Kernel::Memory::PhysicalAddress{physicalPage.PhysicalStart},
                         .Pages = Kernel::Memory::PageCount{1U},
                         .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                         .Kind = Kernel::Memory::FrameKind::Ram,
                         .Reserved = {},
                     });
    if (m_sharedRootShootdownFailure.Frame != Kernel::Memory::FrameError::None)
    {
        return Ia64BspSharedRootShootdownError::FrameInitializationRejected;
    }

    m_sharedRootShootdownFailure.MemoryObject =
        m_sharedRootShootdownMemoryObject.Initialize(m_objectRegistry, m_resourceDomainIdentity, *m_resourceDomain,
                                                     Kernel::Memory::MemoryObjectDescriptor{
                                                         .Pages = Kernel::Memory::PageCount{1U},
                                                         .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                         .MaximumPermissions = SupervisorReadWritePermissions(),
                                                         .Commitment = Kernel::Memory::MemoryCommitment::Pinned,
                                                         .Reserved = {},
                                                     });
    if (m_sharedRootShootdownFailure.MemoryObject.Error != Kernel::Memory::MemoryObjectError::None)
    {
        return Ia64BspSharedRootShootdownError::MemoryObjectInitializationRejected;
    }
    return Ia64BspSharedRootShootdownError::None;
}

Ia64BspSharedRootShootdownError Ia64BspKernelRuntime::RegisterSharedRootShootdownObjects() noexcept
{
    const auto registeredFrame = m_issuer->CreateObject(m_sharedRootShootdownFrame);
    if (!registeredFrame.HasValue())
    {
        m_sharedRootShootdownFailure.Registry = registeredFrame.GetError();
        return Ia64BspSharedRootShootdownError::ObjectRegistrationRejected;
    }
    Kernel::Object::ObjectPin frameVerification;
    m_sharedRootShootdownFailure.Pin =
        m_objectRegistry.PinObject(registeredFrame.GetValue(), Kernel::Object::ObjectType::Frame, frameVerification);
    if ((m_sharedRootShootdownFailure.Pin != Kernel::Object::ObjectPinError::Ok) || !frameVerification.IsValid() ||
        (&frameVerification.GetObject() != &m_sharedRootShootdownFrame))
    {
        return Ia64BspSharedRootShootdownError::RegistryBindingRejected;
    }
    m_sharedRootShootdownSummary.FrameIdentity = Kernel::Memory::FrameId{m_objectRegistry, registeredFrame.GetValue()};
    frameVerification.Reset();

    const auto registeredMemoryObject = m_issuer->CreateObject(m_sharedRootShootdownMemoryObject);
    if (!registeredMemoryObject.HasValue())
    {
        m_sharedRootShootdownFailure.Registry = registeredMemoryObject.GetError();
        return Ia64BspSharedRootShootdownError::ObjectRegistrationRejected;
    }
    Kernel::Object::ObjectPin memoryObjectVerification;
    m_sharedRootShootdownFailure.Pin = m_objectRegistry.PinObject(
        registeredMemoryObject.GetValue(), Kernel::Object::ObjectType::MemoryObject, memoryObjectVerification);
    if ((m_sharedRootShootdownFailure.Pin != Kernel::Object::ObjectPinError::Ok) ||
        !memoryObjectVerification.IsValid() ||
        (&memoryObjectVerification.GetObject() != &m_sharedRootShootdownMemoryObject))
    {
        return Ia64BspSharedRootShootdownError::RegistryBindingRejected;
    }
    m_sharedRootShootdownSummary.MemoryObjectIdentity =
        Kernel::Memory::MemoryObjectId{m_objectRegistry, registeredMemoryObject.GetValue()};
    memoryObjectVerification.Reset();
    return Ia64BspSharedRootShootdownError::None;
}

Ia64BspSharedRootShootdownError Ia64BspKernelRuntime::PrepareSharedRootShootdownMapping() noexcept
{
    m_sharedRootShootdownFailure.MappingPreparation = m_sharedRootShootdownPreparation.Prepare(
        m_objectRegistry, m_sharedRootShootdownSummary.MemoryObjectIdentity, m_sharedRootShootdownMemoryObject,
        m_sharedRootShootdownSummary.FrameIdentity, m_sharedRootShootdownFrame,
        Kernel::Memory::PinnedFrameMappingRequest{
            .VirtualStart = Kernel::Memory::VirtualAddress{Ia64BspSharedRootShootdownVirtualStart},
            .Pages = Kernel::Memory::PageCount{1U},
            .MemoryObjectOffsetPages = 0U,
            .FrameOffsetPages = 0U,
            .Permissions = SupervisorReadWritePermissions(),
            .Reserved = {},
        });
    if (m_sharedRootShootdownFailure.MappingPreparation != Kernel::Memory::PinnedFrameMappingError::None)
    {
        return Ia64BspSharedRootShootdownError::MappingPreparationRejected;
    }
    return Ia64BspSharedRootShootdownError::None;
}

Ia64BspSharedRootShootdownError
Ia64BspKernelRuntime::DispatchSharedRootShootdown(Hal::Memory::ITlbShootdownTransport& transport) noexcept
{
    const auto dispatched =
        m_tlbShootdownDispatcher->Dispatch(*m_root.AddressSpaceStorage, m_sharedRootShootdownRequest, transport);
    if (!dispatched.HasValue())
    {
        m_sharedRootShootdownFailure.Shootdown = dispatched.GetError();
        return Ia64BspSharedRootShootdownError::ShootdownRejected;
    }
    m_sharedRootShootdownReceipt = dispatched.GetValue();
    const Foundation::ProcessorMask bspBit = ProcessorBit(m_root.Processor);
    const Foundation::ProcessorMask queuedProcessorMask = static_cast<Foundation::UInt8>(
        m_sharedRootShootdownSummary.TargetProcessorMask & static_cast<Foundation::ProcessorMask>(~bspBit));
    if (!m_sharedRootShootdownReceipt.Token.IsEqualTo(m_sharedRootShootdownRequest.GetToken()) ||
        (m_sharedRootShootdownReceipt.QueuedProcessorMask != queuedProcessorMask) ||
        (m_sharedRootShootdownReceipt.AcknowledgedProcessorMask != bspBit) ||
        !m_sharedRootShootdownReceipt.Processor.IsValid() ||
        (m_sharedRootShootdownReceipt.Processor.GetValue() != 0U) || m_sharedRootShootdownReceipt.RejectionObserved ||
        !BytesAreZero(m_sharedRootShootdownReceipt.Reserved, sizeof(m_sharedRootShootdownReceipt.Reserved)))
    {
        return Ia64BspSharedRootShootdownError::ShootdownReceiptMismatch;
    }
    m_sharedRootShootdownSummary.QueuedProcessorMask = queuedProcessorMask;
    m_sharedRootShootdownSummary.AcknowledgedProcessorMask = bspBit;
    return Ia64BspSharedRootShootdownError::None;
}

Ia64BspSharedRootShootdownError Ia64BspKernelRuntime::WaitForSharedRootShootdown() noexcept
{
    const Kernel::Memory::AddressSpaceMappingToken token = m_sharedRootShootdownRequest.GetToken();
    const Foundation::ProcessorMask bspBit = ProcessorBit(m_root.Processor);
    const Foundation::ProcessorMask target = m_sharedRootShootdownSummary.TargetProcessorMask;
    for (Kernel::Scheduler::CrossProcessorWait wait;;)
    {
        m_sharedRootShootdownObservedRoot = m_root.AddressSpaceStorage->GetSnapshot();
        if ((m_sharedRootShootdownObservedRoot.State != Kernel::Memory::AddressSpaceState::ShootdownPending) ||
            !m_sharedRootShootdownObservedRoot.Identity.IsEqualTo(m_root.AddressSpaceIdentity) ||
            (m_sharedRootShootdownObservedRoot.ResidentProcessorMask != target) ||
            (m_sharedRootShootdownObservedRoot.ShootdownSequence != token.GetSequence()) ||
            (m_sharedRootShootdownObservedRoot.TargetProcessorMask != target) ||
            ((m_sharedRootShootdownObservedRoot.AcknowledgedProcessorMask & bspBit) == 0U) ||
            ((m_sharedRootShootdownObservedRoot.AcknowledgedProcessorMask &
              static_cast<Foundation::ProcessorMask>(~target)) != 0U) ||
            !BytesAreZero(m_sharedRootShootdownObservedRoot.Reserved,
                          sizeof(m_sharedRootShootdownObservedRoot.Reserved)))
        {
            return Ia64BspSharedRootShootdownError::ShootdownStateMismatch;
        }
        m_sharedRootShootdownSummary.AcknowledgedProcessorMask =
            m_sharedRootShootdownObservedRoot.AcknowledgedProcessorMask;
        if (m_sharedRootShootdownObservedRoot.AcknowledgedProcessorMask == target)
        {
            return Ia64BspSharedRootShootdownError::None;
        }
        if (!wait.Poll())
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(
                target & ~m_sharedRootShootdownObservedRoot.AcknowledgedProcessorMask);
        }
    }
}

bool Ia64BspKernelRuntime::ValidateSharedRootShootdownPostconditions() noexcept
{
    m_sharedRootShootdownObservedRoot = m_root.AddressSpaceStorage->GetSnapshot();
    m_sharedRootShootdownFinalResources = m_resourceDomain->GetSnapshot();
    const Kernel::Memory::FrameSnapshot frame = m_sharedRootShootdownFrame.GetSnapshot();
    const Kernel::Memory::MemoryObjectSnapshot memoryObject = m_sharedRootShootdownMemoryObject.GetSnapshot();
    const auto mapping = m_root.AddressSpaceStorage->TryGetMapping(m_sharedRootShootdownInitialRoot.MappingCount);
    const Kernel::Memory::PagePermissions permissions = SupervisorReadWritePermissions();
    return (m_sharedRootShootdownObservedRoot.State == Kernel::Memory::AddressSpaceState::Ready) &&
           m_sharedRootShootdownObservedRoot.Identity.IsEqualTo(m_sharedRootShootdownSummary.AddressSpaceIdentity) &&
           (m_sharedRootShootdownObservedRoot.MappingCount == (m_sharedRootShootdownInitialRoot.MappingCount + 1U)) &&
           (m_sharedRootShootdownObservedRoot.ResidentProcessorMask ==
            m_sharedRootShootdownSummary.TargetProcessorMask) &&
           (m_sharedRootShootdownObservedRoot.ShootdownSequence == 0U) &&
           (m_sharedRootShootdownObservedRoot.TransitionSequence == 0U) &&
           (m_sharedRootShootdownObservedRoot.TargetProcessorMask == 0U) &&
           (m_sharedRootShootdownObservedRoot.AcknowledgedProcessorMask == 0U) &&
           BytesAreZero(m_sharedRootShootdownObservedRoot.Reserved,
                        sizeof(m_sharedRootShootdownObservedRoot.Reserved)) &&
           (m_sharedRootShootdownSummary.Mapping.MappingSequence ==
            m_sharedRootShootdownRequest.GetToken().GetSequence()) &&
           (m_sharedRootShootdownSummary.Mapping.MappingCount == m_sharedRootShootdownObservedRoot.MappingCount) &&
           (m_sharedRootShootdownSummary.Mapping.VirtualStart.GetValue() == Ia64BspSharedRootShootdownVirtualStart) &&
           (m_sharedRootShootdownSummary.Mapping.Pages.GetValue() == 1U) && mapping.HasValue() &&
           SharedRootMappingMatches(mapping.GetValue(), m_sharedRootShootdownSummary.MemoryObjectIdentity,
                                    m_sharedRootShootdownSummary.FrameIdentity,
                                    m_sharedRootShootdownPhysicalPage.PhysicalStart) &&
           (frame.State == Kernel::Memory::FrameState::Ready) &&
           (frame.Descriptor.PhysicalStart.GetValue() == m_sharedRootShootdownPhysicalPage.PhysicalStart) &&
           (frame.Descriptor.Pages.GetValue() == 1U) &&
           (frame.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (frame.Descriptor.Kind == Kernel::Memory::FrameKind::Ram) &&
           BytesAreZero(frame.Descriptor.Reserved, sizeof(frame.Descriptor.Reserved)) &&
           BytesAreZero(frame.Reserved, sizeof(frame.Reserved)) &&
           (memoryObject.State == Kernel::Memory::MemoryObjectState::Ready) &&
           (memoryObject.Descriptor.Pages.GetValue() == 1U) &&
           (memoryObject.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (memoryObject.Descriptor.MaximumPermissions.GetBits() == permissions.GetBits()) &&
           (memoryObject.Descriptor.Commitment == Kernel::Memory::MemoryCommitment::Pinned) &&
           BytesAreZero(memoryObject.Descriptor.Reserved, sizeof(memoryObject.Descriptor.Reserved)) &&
           memoryObject.ResourceDomainIdentity.IsEqualTo(m_resourceDomainIdentity) &&
           (memoryObject.ResourceDomain.GetValue() == m_resourceDomain->GetIdentifier().GetValue()) &&
           (memoryObject.ActiveMappings == 1U) && BytesAreZero(memoryObject.Reserved, sizeof(memoryObject.Reserved)) &&
           (m_objectRegistry.GetActiveCount() == (m_sharedRootShootdownInitialRegistryCount + 2U)) &&
           ResourceUsageHasExactPageIncrease(m_sharedRootShootdownInitialResources,
                                             m_sharedRootShootdownFinalResources);
}

Ia64BspKernelRuntime::SharedRootShootdownResult
Ia64BspKernelRuntime::MapSharedRootShootdownPage(const Kernel::Memory::PhysicalPageRange& physicalPage,
                                                 const Foundation::UInt8 profiledProcessorMask,
                                                 Hal::Memory::ITlbShootdownTransport& transport) noexcept
{
    if (m_sharedRootShootdownState != Ia64BspSharedRootShootdownState::Fresh)
    {
        return SharedRootShootdownResult::Failure(Ia64BspSharedRootShootdownError::AlreadyAttempted);
    }
    Ia64BspSharedRootShootdownError error = PreflightSharedRootShootdown(physicalPage, profiledProcessorMask);
    if (error != Ia64BspSharedRootShootdownError::None)
    {
        return SharedRootShootdownResult::Failure(error);
    }

    m_sharedRootShootdownState = Ia64BspSharedRootShootdownState::Mapping;
    m_sharedRootShootdownSummary.State = Ia64BspSharedRootShootdownState::Mapping;
    m_sharedRootShootdownPhysicalPage = physicalPage;
    error = InitializeSharedRootShootdownObjects(physicalPage);
    if (error == Ia64BspSharedRootShootdownError::None)
    {
        error = RegisterSharedRootShootdownObjects();
    }
    if (error == Ia64BspSharedRootShootdownError::None)
    {
        error = PrepareSharedRootShootdownMapping();
    }
    if (error != Ia64BspSharedRootShootdownError::None)
    {
        return FailSharedRootShootdown(error);
    }

    const auto begin = m_root.AddressSpaceStorage->BeginPinnedFrameMapping(m_root.AddressSpaceIdentity,
                                                                           m_sharedRootShootdownPreparation);
    if (!begin.HasValue())
    {
        m_sharedRootShootdownFailure.AddressSpace = begin.GetError();
        return FailSharedRootShootdown(Ia64BspSharedRootShootdownError::AddressSpaceMappingRejected);
    }
    m_sharedRootShootdownRequest = begin.GetValue();
    m_sharedRootShootdownSummary.ShootdownSequence = m_sharedRootShootdownRequest.GetToken().GetSequence();
    error = DispatchSharedRootShootdown(transport);
    if (error == Ia64BspSharedRootShootdownError::ShootdownRejected)
    {
        return FailSharedRootShootdown(error);
    }
    if (error != Ia64BspSharedRootShootdownError::None)
    {
        return FailPendingSharedRootShootdown(error);
    }

    error = WaitForSharedRootShootdown();
    if (error != Ia64BspSharedRootShootdownError::None)
    {
        return FailPendingSharedRootShootdown(error);
    }
    const auto completed = m_root.AddressSpaceStorage->CompleteMapping(m_sharedRootShootdownRequest.GetToken());
    if (!completed.HasValue())
    {
        m_sharedRootShootdownFailure.AddressSpace = completed.GetError();
        return FailSharedRootShootdown(Ia64BspSharedRootShootdownError::MappingCompletionRejected);
    }
    m_sharedRootShootdownSummary.Mapping = completed.GetValue();
    if (!ValidateSharedRootShootdownPostconditions())
    {
        return FailSharedRootShootdown(Ia64BspSharedRootShootdownError::PostconditionRejected);
    }

    m_sharedRootShootdownState = Ia64BspSharedRootShootdownState::Complete;
    m_sharedRootShootdownSummary.State = Ia64BspSharedRootShootdownState::Complete;
    return SharedRootShootdownResult::Success(m_sharedRootShootdownSummary);
}

UserProbePreparationResult
Ia64BspKernelRuntime::PrepareUserExecutionProbe(Ia64UserExecutionProbe& userExecutionProbe) noexcept
{
    if (m_initialUserImageState != Ia64BspKernelRuntimeInitialUserImageState::Fresh)
    {
        return UserProbePreparationResult::Failure(Ia64BspKernelRuntimeUserProbeError::ConfigurationConflict);
    }
    if (m_userProbeState == Ia64BspKernelRuntimeUserProbeState::Prepared ||
        m_userProbeState == Ia64BspKernelRuntimeUserProbeState::Dispatching ||
        m_userProbeState == Ia64BspKernelRuntimeUserProbeState::DispatchFailed)
    {
        return UserProbePreparationResult::Failure(Ia64BspKernelRuntimeUserProbeError::AlreadyPrepared);
    }
    if (m_userProbeState == Ia64BspKernelRuntimeUserProbeState::PreparationFailed)
    {
        return UserProbePreparationResult::Failure(Ia64BspKernelRuntimeUserProbeError::PreviousPreparationFailed);
    }
    if (m_state != Ia64BspKernelRuntimeState::Prepared)
    {
        return UserProbePreparationResult::Failure(Ia64BspKernelRuntimeUserProbeError::RuntimeNotPrepared);
    }
    if ((m_runtimeMemory == nullptr) || (m_resourceDomain == nullptr) || (m_issuer == nullptr) ||
        !m_resourceDomainIdentity.IsValid() || !m_threadIdentity.IsValid())
    {
        return FailUserProbePreparation(Ia64BspKernelRuntimeUserProbeError::InvalidRetainedDependencies);
    }
    if (!ValidateUserProbePlacement(userExecutionProbe))
    {
        return UserProbePreparationResult::Failure(Ia64BspKernelRuntimeUserProbeError::InvalidProbePlacement);
    }
    if (!SwitchToInitialComponentRoot())
    {
        return FailUserProbePreparation(Ia64BspKernelRuntimeUserProbeError::PrivateRootActivationRejected);
    }
    Ia64BspKernelRuntimeUserProbeError preparationError{Ia64BspKernelRuntimeUserProbeError::None};
    if (!ValidateSoleResidentRoot())
    {
        preparationError = Ia64BspKernelRuntimeUserProbeError::SoleResidentRootRejected;
    }
    else if (!PrepareUserProbeLaunchRecord(userExecutionProbe))
    {
        preparationError = Ia64BspKernelRuntimeUserProbeError::ProbePreparationRejected;
    }
    else if (!InitializeSystemCallWaitContinuation())
    {
        preparationError = Ia64BspKernelRuntimeUserProbeError::WaitContinuationRejected;
    }
    else if (!PrepareUserProbeCapability())
    {
        preparationError = Ia64BspKernelRuntimeUserProbeError::CapabilityAuthorityRejected;
    }
    else if (!InitializeUserProbeEndpoint())
    {
        preparationError = m_userProbeFailure.Error;
    }
    else if (!PrepareUserProbeWaitSetCapability())
    {
        preparationError = m_userProbeFailure.Error;
    }
    else if (!InitializeUserMemory())
    {
        preparationError = Ia64BspKernelRuntimeUserProbeError::UserMemoryInitializationRejected;
    }
    else if (!PrepareUserProbeBootstrap())
    {
        preparationError = m_userProbeFailure.Error;
    }
    else if (!ValidateUserProbeIpcPostconditions())
    {
        preparationError = Ia64BspKernelRuntimeUserProbeError::PreparationPostconditionRejected;
    }
    if (!SwitchToCommonRoot())
    {
        return FailUserProbePreparation(Ia64BspKernelRuntimeUserProbeError::CommonRootRestorationRejected);
    }
    if (preparationError != Ia64BspKernelRuntimeUserProbeError::None)
    {
        return FailUserProbePreparation(preparationError);
    }

    m_userProbeState = Ia64BspKernelRuntimeUserProbeState::Prepared;
    return UserProbePreparationResult::Success(Ia64BspKernelRuntimeUserProbePreparationSummary{
        .ThreadIdentity = m_threadIdentity,
        .Processor = m_root.Processor,
        .State = m_userProbeState,
        .Reserved = {},
    });
}

InitialUserImagePreparationResult
Ia64BspKernelRuntime::PrepareInitialUserImage(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                                              const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    if (m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::Prepared ||
        m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::Dispatching ||
        m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::DispatchFailed)
    {
        return InitialUserImagePreparationResult::Failure(Ia64BspKernelRuntimeInitialUserImageError::AlreadyPrepared);
    }
    if (m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::PreparationFailed)
    {
        return InitialUserImagePreparationResult::Failure(
            Ia64BspKernelRuntimeInitialUserImageError::PreviousPreparationFailed);
    }
    if (m_userProbeState != Ia64BspKernelRuntimeUserProbeState::Fresh)
    {
        return InitialUserImagePreparationResult::Failure(
            Ia64BspKernelRuntimeInitialUserImageError::ConfigurationConflict);
    }
    if (m_state != Ia64BspKernelRuntimeState::Prepared)
    {
        return InitialUserImagePreparationResult::Failure(
            Ia64BspKernelRuntimeInitialUserImageError::RuntimeNotPrepared);
    }
    if ((m_runtimeMemory == nullptr) || (m_resourceDomain == nullptr) || (m_issuer == nullptr) ||
        !m_resourceDomainIdentity.IsValid() || !m_protectionDomainIdentity.IsValid() || !m_threadIdentity.IsValid() ||
        !ValidateBorrowedInitialComponentRoot())
    {
        return FailInitialUserImagePreparation(Ia64BspKernelRuntimeInitialUserImageError::InvalidRetainedDependencies,
                                               false);
    }

    m_initialUserImageFailure = {};
    Ia64BspKernelRuntimeInitialUserImageError preparationError =
        CaptureInitialUserImageMemoryMappings(memoryOwner, mappingOwner);
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, false);
    }
    preparationError = ReserveInitialUserImageResources();
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, m_initialUserImageResourceReservationAccepted);
    }
    preparationError = InitializeInitialUserImageEndpoint();
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, true);
    }
    preparationError = InitializeInitialUserImageDiagnosticChannel();
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, true);
    }
    preparationError = InitializeInitialUserImageMemory();
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, true);
    }
    preparationError = PrepareInitialUserImageLaunchOwner(memoryOwner, mappingOwner);
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, true);
    }
    preparationError = RetainInitialUserImageLaunchRequest();
    if (preparationError != Ia64BspKernelRuntimeInitialUserImageError::None)
    {
        return FailInitialUserImagePreparation(preparationError, true);
    }
    if (!ValidateInitialUserImageIpcPostconditions() || !ValidateInitialUserImageDiagnosticChannelPostconditions())
    {
        return FailInitialUserImagePreparation(
            Ia64BspKernelRuntimeInitialUserImageError::PreparationPostconditionRejected, true);
    }

    m_initialUserImageState = Ia64BspKernelRuntimeInitialUserImageState::Prepared;
    return InitialUserImagePreparationResult::Success(Ia64BspKernelRuntimeInitialUserImagePreparationSummary{
        .ThreadIdentity = m_threadIdentity,
        .Processor = m_root.Processor,
        .State = m_initialUserImageState,
        .Reserved = {},
    });
}

LocalTimerProbeResult Ia64BspKernelRuntime::RunLocalTimerInterruptProbe() noexcept
{
    if (m_localTimerProbeState != Ia64BspKernelRuntimeLocalTimerProbeState::Fresh)
    {
        return LocalTimerProbeResult::Failure(Ia64BspKernelRuntimeLocalTimerProbeError::AlreadyAttempted);
    }
    if ((m_state != Ia64BspKernelRuntimeState::Prepared) || (m_runtimeMemory == nullptr) || !m_root.Processor.IsValid())
    {
        return LocalTimerProbeResult::Failure(Ia64BspKernelRuntimeLocalTimerProbeError::RuntimeNotPrepared);
    }
    if ((m_userProbeState != Ia64BspKernelRuntimeUserProbeState::Prepared) || !m_userMemory.IsReady())
    {
        return LocalTimerProbeResult::Failure(Ia64BspKernelRuntimeLocalTimerProbeError::UserMemoryNotPrepared);
    }

    m_localTimerProbeSummary = {};
    m_localTimerProbeFailure = {};
    m_localTimerProbeEvidence = {};
    m_localTimerProbeTimerQueueEvidence = {};
    m_localTimerProbeArmReceipt = {};
    m_localTimerProbeInitialDeadlineReceipt = {};
    m_localTimerProbeDeliveryReceipt = {};
    m_localTimerProbeFinalNotificationSnapshot = {};
    m_localTimerProbeFinalDeadlineReceipt = {};
    m_localTimerProbePublication = {};
    m_localTimerProbePollingRange = {};
    m_localTimerProbePublishedContextAddress = 0U;
    m_localTimerProbePublishedEvidenceAddress = 0U;
    m_localTimerProbeNotificationClaimSequence = 0U;
    if (!PrepareLocalTimerProbeAttempt())
    {
        const Ia64BspKernelRuntimeLocalTimerProbeError preparationError = m_localTimerProbeFailure.Error;
        if (HasPossiblyAcceptedLocalTimerProbeArm() && !RollbackLocalTimerProbeBeforeCompletion())
        {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
            static_cast<void>(FailLocalTimerProbe(m_localTimerProbeFailure.Error));
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::TimerPreparationRollback);
#else
            return FailLocalTimerProbe(m_localTimerProbeFailure.Error);
#endif
        }
        return FailLocalTimerProbe(preparationError);
    }

    m_localTimerProbeFailure.PollStatus = AsteriaIa64PollForExternalInterrupt(
        m_localTimerProbePublishedEvidenceAddress, m_localTimerProbeFailure.AbsoluteTimeoutTicks);
    if (m_localTimerProbeFailure.PollStatus != Interrupt::Ia64ExternalInterruptPollStatus::Completed)
    {
        if (!RollbackLocalTimerProbeBeforeCompletion())
        {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
            static_cast<void>(FailLocalTimerProbe(m_localTimerProbeFailure.Error));
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::TimerPollRollback);
#else
            return FailLocalTimerProbe(m_localTimerProbeFailure.Error);
#endif
        }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        static_cast<void>(FailLocalTimerProbe(Ia64BspKernelRuntimeLocalTimerProbeError::PollRejected));
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::TimerPoll);
#else
        return FailLocalTimerProbe(Ia64BspKernelRuntimeLocalTimerProbeError::PollRejected);
#endif
    }

    // The unchanged entry has returned through rfi with PSR.i clear and ITV
    // masked. Normal context now owns every lock-taking delivery transition.
    if (!DeliverLocalTimerProbeExpiration())
    {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        static_cast<void>(FailLocalTimerProbe(m_localTimerProbeFailure.Error));
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::TimerDelivery);
#else
        return FailLocalTimerProbe(m_localTimerProbeFailure.Error);
#endif
    }

    const bool notificationConsumed = ConsumeLocalTimerProbeNotification();
    const Ia64BspKernelRuntimeLocalTimerProbeError notificationError = m_localTimerProbeFailure.Error;
    if (!RefreshEmptyLocalTimerProbeDeadline())
    {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        static_cast<void>(FailLocalTimerProbe(m_localTimerProbeFailure.Error));
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::TimerRefresh);
#else
        return FailLocalTimerProbe(m_localTimerProbeFailure.Error);
#endif
    }
    if (!notificationConsumed)
    {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        static_cast<void>(FailLocalTimerProbe(notificationError));
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::TimerNotification);
#else
        return FailLocalTimerProbe(notificationError);
#endif
    }

    if (!CompleteLocalTimerProbeAttempt())
    {
        return FailLocalTimerProbe(m_localTimerProbeFailure.Error);
    }

    m_localTimerProbeState = Ia64BspKernelRuntimeLocalTimerProbeState::Complete;
    if (!PrepareUserProbeWaitPath())
    {
        return FailLocalTimerProbe(Ia64BspKernelRuntimeLocalTimerProbeError::WaitPathRejected);
    }
    m_localTimerProbeSummary = Ia64BspKernelRuntimeLocalTimerProbeSummary{
        .ProgrammedDeadlineTicks = m_localTimerProbeFailure.ProgrammedDeadlineTicks,
        .ObservedIntervalTimeCounter = m_localTimerProbeEvidence.ObservedIntervalTimeCounter,
        .Processor = m_root.Processor,
        .State = m_localTimerProbeState,
        .Reserved = {},
    };
    return LocalTimerProbeResult::Success(m_localTimerProbeSummary);
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::BindWorker() noexcept
{
    m_failure.RunLoopError = RunLoop().Bind(m_thread, m_workerContext, m_protectionDomain);
    return m_failure.RunLoopError == Kernel::Scheduler::ProcessorKernelRunLoopError::None;
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::EnqueueWorker() noexcept
{
    m_dispatchFailure = {};
    if (RunLoop().EnqueueBoundThread(m_schedulingContextIdentity, m_schedulingContext, m_threadIdentity, m_thread,
                                     m_dispatchFailure))
    {
        return true;
    }
    m_failure.RunLoopError = m_dispatchFailure.Error;
    m_failure.ControllerError = m_dispatchFailure.ControllerError;
    m_failure.SchedulerError = m_dispatchFailure.SchedulerError;
    m_failure.DeadlineError = m_dispatchFailure.DeadlineError;
    m_failure.AddressSpaceControllerError = m_dispatchFailure.AddressSpaceController.Error;
    return false;
}

ASTERIA_NOINLINE_NOIPA bool Ia64BspKernelRuntime::DispatchWorkerFromIdle() noexcept
{
    return RunLoop().DispatchFromIdle(m_dispatchReceipt, m_dispatchFailure);
}

bool Ia64BspKernelRuntime::ValidateExpectedDispatchFailure(
    const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) const noexcept
{
    return (failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected) &&
           (failure.ControllerError == Kernel::Scheduler::ProcessorRunError::NoRunnableThread) &&
           (failure.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread) &&
           (failure.DeadlineError == Kernel::Time::ProcessorDeadlineError::None) &&
           (failure.DeadlineReceipt.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (failure.DeadlineReceipt.SelectedDeadlineTicks == 0U) && (failure.DeadlineReceipt.SelectedSources == 0U) &&
           (failure.DeadlineReceipt.Reprogrammed == 1U) && (failure.ActiveRunRetained == 0U) &&
           (failure.NativeError == 0U) &&
           (failure.AddressSpaceController.Error == Kernel::Memory::ProcessorAddressSpaceControllerError::None);
}

bool Ia64BspKernelRuntime::ValidateDispatchOwnerState(const Foundation::UInt64 expectedExitCode) const noexcept
{
    if (!ValidateBorrowedRoot())
    {
        return false;
    }
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    const Kernel::Time::TimerQueueState timerQueue = m_timerQueue.GetState();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = RunLoop().GetSnapshot();
    const Kernel::Scheduler::ThreadSnapshot thread = m_thread.GetSnapshot();
    return (timer.State == Time::Ia64LocalTimerState::Ready) && (timer.Vector == m_timerVector) &&
           (timer.Armed == 0U) && (timer.ProgrammedDeadlineTicks == 0U) && (timerQueue.NextDeadlineTicks == 0U) &&
           (timerQueue.ScheduledEntries == 0U) && (timerQueue.PendingEntries == 0U) &&
           (timerQueue.ClaimedEntries == 0U) && (controller.State == Kernel::Scheduler::ProcessorRunState::Idle) &&
           (controller.Mode == Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Disarmed) &&
           (controller.Deadline.SelectedDeadlineTicks == 0U) && (controller.Deadline.SelectedSources == 0U) &&
           (runLoop.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) &&
           (runLoop.IdleFailurePending == 0U) && (runLoop.ActiveThread == nullptr) &&
           (runLoop.ActiveProtectionDomain == nullptr) && (runLoop.ActiveAddressSpace == nullptr) &&
           (runLoop.BindingCount == 1U) && (thread.State == Kernel::Scheduler::ThreadState::Exited) &&
           (thread.ExitCode == expectedExitCode);
}

bool Ia64BspKernelRuntime::ValidateUserProbeDispatchOwnerState() const noexcept
{
    return ValidateDispatchOwnerState(Ia64BspUserProbeLaunchFailureExitCode) &&
           (m_userProbeState == Ia64BspKernelRuntimeUserProbeState::DispatchFailed) &&
           (m_userProbeFailure.Error == Ia64BspKernelRuntimeUserProbeError::LaunchRejected) &&
           (m_userProbeFailure.ExitCode == Ia64BspUserProbeLaunchFailureExitCode) &&
           IsRecoverableUserProbeLaunchError(m_userProbeFailure.Launch) && (m_userExecutionProbe != nullptr) &&
           (m_userExecutionProbe->GetState() == Ia64UserExecutionProbeState::Ready) &&
           (m_userProbeLaunchRequest.SupervisorContinuation != nullptr) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->State == Ia64UserModeSupervisorContinuationState::Fresh) &&
           (m_userProbeLaunchRequest.SupervisorContinuation->Owner == Ia64UserModeSupervisorContinuationOwner::None);
}

void Ia64BspKernelRuntime::RetainUserProbeDispatchFailure() noexcept
{
    m_userProbeFailure.RunLoop = m_dispatchFailure.Error;
    m_userProbeFailure.Controller = m_dispatchFailure.ControllerError;
    m_userProbeFailure.Scheduler = m_dispatchFailure.SchedulerError;
    m_userProbeFailure.Deadline = m_dispatchFailure.DeadlineError;
    m_userProbeFailure.AddressSpaceController = m_dispatchFailure.AddressSpaceController.Error;
}

bool Ia64BspKernelRuntime::ValidateInitialUserImageDispatchOwnerState() const noexcept
{
    return ValidateDispatchOwnerState(Ia64BspInitialUserImageLaunchFailureExitCode) &&
           (m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::DispatchFailed) &&
           (m_initialUserImageFailure.Error == Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected) &&
           (m_initialUserImageFailure.ExitCode == Ia64BspInitialUserImageLaunchFailureExitCode) &&
           IsRecoverableUserProbeLaunchError(m_initialUserImageFailure.Launch) &&
           (m_initialUserImageLaunchOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Ready) &&
           (m_initialUserImageLaunchRequest.SupervisorContinuation != nullptr) &&
           (m_initialUserImageLaunchRequest.SupervisorContinuation->State ==
            Ia64UserModeSupervisorContinuationState::Fresh) &&
           (m_initialUserImageLaunchRequest.SupervisorContinuation->Owner ==
            Ia64UserModeSupervisorContinuationOwner::None);
}

void Ia64BspKernelRuntime::RetainInitialUserImageDispatchFailure() noexcept
{
    m_initialUserImageFailure.RunLoop = m_dispatchFailure.Error;
    m_initialUserImageFailure.Controller = m_dispatchFailure.ControllerError;
    m_initialUserImageFailure.Scheduler = m_dispatchFailure.SchedulerError;
    m_initialUserImageFailure.Deadline = m_dispatchFailure.DeadlineError;
    m_initialUserImageFailure.AddressSpaceController = m_dispatchFailure.AddressSpaceController.Error;
}

bool Ia64BspKernelRuntime::IsRecoverableUserProbeLaunchError(const Ia64UserModeLaunchError error) noexcept
{
    return (error == Ia64UserModeLaunchError::UnsupportedProcessorState) ||
           (error == Ia64UserModeLaunchError::InvalidActiveSupervisorState) ||
           (error == Ia64UserModeLaunchError::NativeTransitionUnavailable);
}

bool Ia64BspKernelRuntime::ValidateWorkerEvidence() const noexcept
{
    const Foundation::UInt64 stackStart = AddressOf(m_workerMemoryStack);
    const Foundation::UInt64 stackEnd = stackStart + sizeof(m_workerMemoryStack);
    const Foundation::UInt64 rseStart = AddressOf(m_workerRseBackingStore);
    const Foundation::UInt64 rseEnd = rseStart + sizeof(m_workerRseBackingStore);
    return (m_workerEvidence.CaptureComplete == 1U) && (m_workerEvidence.StackPointer >= stackStart) &&
           (m_workerEvidence.StackPointer < stackEnd) && (m_workerEvidence.RseBackingStorePointer >= rseStart) &&
           (m_workerEvidence.RseBackingStorePointer < rseEnd) && (m_workerEvidence.RseBackingStoreBase >= rseStart) &&
           (m_workerEvidence.RseBackingStoreBase < rseEnd) &&
           (m_workerEvidence.RseBackingStorePointer >= m_workerEvidence.RseBackingStoreBase) &&
           (m_workerEvidence.RseConfiguration == 0U) &&
           ((m_workerEvidence.ProcessorStatus & TranslationStateMask) == TranslationStateMask) &&
           ((m_workerEvidence.ProcessorStatus & InterruptionEnableMask) == 0U);
}

#if ASTERIA_IA64_BSP_NATIVE_PREEMPTION_PROBE_CODE
bool Ia64BspKernelRuntime::ArmNativePreemptionSentinel() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    if ((m_nativePreemptionControl == nullptr) || (m_state != Ia64BspKernelRuntimeState::Dispatching) ||
        (m_workerPurpose != WorkerPurpose::NativeExternalPreemption))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionArmRejected;
        return false;
    }

    Interrupt::Ia64ExternalPreemptionFrame& frame = m_workerContext.GetExternalPreemptionFrame();
    m_nativePreemptionControlSnapshot =
        Interrupt::AcquireIa64ExternalPreemptionControlSnapshot(*m_nativePreemptionControl);
    const Interrupt::Ia64ExternalPreemptionControlSnapshot& control = m_nativePreemptionControlSnapshot;
    const auto active = RunLoop().GetActiveExecutionBinding();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    const Foundation::UInt8 runStopSource =
        static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::RunStop);
    if (!active.HasValue() || (active.GetValue().ThreadStorage != &m_thread) ||
        (active.GetValue().SchedulingContextStorage != &m_nativePreemptionSchedulingContext) ||
        !active.GetValue().ThreadIdentity.IsEqualTo(m_threadIdentity) ||
        !active.GetValue().SchedulingContextIdentity.IsEqualTo(m_nativePreemptionSchedulingContextIdentity) ||
        (active.GetValue().Processor.GetValue() != m_root.Processor.GetValue()) ||
        (active.GetValue().RunSequence == 0U) ||
        (control.State != Interrupt::Ia64ExternalPreemptionControlState::Configured) ||
        (control.Mode != Interrupt::Ia64ExternalPreemptionMode::LegacyEvidence) ||
        (control.Configuration.FrameAddress != AddressOf(&frame)) ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Active) ||
        (controller.Mode != Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Armed) ||
        (controller.Deadline.SelectedDeadlineTicks == 0U) ||
        (controller.Deadline.SelectedDeadlineTicks == MaximumUInt64) ||
        (controller.Deadline.SelectedSources != runStopSource) || (controller.TimerQueue.NextDeadlineTicks != 0U) ||
        (controller.TimerQueue.ScheduledEntries != 0U) || (controller.TimerQueue.PendingEntries != 0U) ||
        (controller.TimerQueue.ClaimedEntries != 0U) || (timer.State != Time::Ia64LocalTimerState::Ready) ||
        (timer.Vector != m_timerVector) || (timer.Armed != 1U) ||
        (timer.ProgrammedDeadlineTicks != controller.Deadline.SelectedDeadlineTicks) ||
        Interrupt::Ia64ExternalPreemptionFrameIsOwned(frame))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionArmRejected;
        return false;
    }

    Interrupt::Ia64ExternalPreemptionSentinelObservation observation{};
    AsteriaIa64CaptureBspNativePreemptionObservation(&observation);
    if ((observation.ProgrammedDeadlineTicks != controller.Deadline.SelectedDeadlineTicks) ||
        (observation.InstructionPointer != m_nativePreemptionSentinelEntryPoint))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionArmRejected;
        return false;
    }
    m_failure.ExternalPreemptionControlError =
        Interrupt::ArmIa64ExternalPreemptionControlForSentinel(*m_nativePreemptionControl, frame, observation);
    if (m_failure.ExternalPreemptionControlError != Interrupt::Ia64ExternalPreemptionControlError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionArmRejected;
        return false;
    }

    if (AsteriaIa64RunBspNativePreemptionSentinel(m_nativePreemptionControl) != 0U)
    {
        m_failure.ExternalPreemptionControlError = Interrupt::Ia64ExternalPreemptionControlError::InvalidSentinelState;
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionArmRejected;
        return false;
    }
    return true;
#else
    m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionUnavailable;
    return false;
#endif
}

bool Ia64BspKernelRuntime::ValidateNativePreemptionCapture(Interrupt::Ia64ExternalPreemptionFrame& frame,
                                                           Foundation::UInt64& frameToken,
                                                           Foundation::UInt64& stoppedRunSequence) noexcept
{
    frameToken = 0U;
    stoppedRunSequence = 0U;
    const Foundation::UInt64 acquiredFrameToken = Interrupt::AcquireIa64ExternalPreemptionToken(frame);
    m_nativePreemptionControlSnapshot =
        Interrupt::AcquireIa64ExternalPreemptionControlSnapshot(*m_nativePreemptionControl);
    const Interrupt::Ia64ExternalPreemptionControlSnapshot& control = m_nativePreemptionControlSnapshot;
    const Foundation::UInt64 interruptedInstructionSlot =
        (frame.InterruptedProcessorStatus & Interrupt::Ia64ExternalInterruptInstructionSlotMask) >> 41U;
    if ((control.State != Interrupt::Ia64ExternalPreemptionControlState::Coordinator) ||
        (control.Mode != Interrupt::Ia64ExternalPreemptionMode::BspOneShotPreemption) ||
        (control.Configuration.ProcessorIndex != m_root.Processor.GetValue()) ||
        (control.Configuration.TimerVector != m_timerVector) ||
        (control.Configuration.FrameAddress != AddressOf(&frame)) ||
        (control.CapturedFrameToken != acquiredFrameToken) ||
        (control.ArmedDeadlineTicks != frame.ProgrammedDeadlineTicks) || (control.CaptureCount != 1U) ||
        (control.RestoreCount != 0U) || (acquiredFrameToken == 0U) || (frame.CaptureToken != acquiredFrameToken) ||
        (frame.State != Interrupt::Ia64ExternalPreemptionFrameState::Retained) ||
        (frame.CaptureStage != Interrupt::Ia64ExternalPreemptionCaptureStage::TimerAcknowledged) ||
        (frame.ProcessorIndex != m_root.Processor.GetValue()) || (frame.TimerVector != m_timerVector) ||
        (frame.ClaimedVector != m_timerVector) ||
        (frame.MaskedTimerVector != Interrupt::Ia64ExternalInterruptMaskedTimerVector) || (frame.EoiCount != 1U) ||
        (frame.ProgrammedDeadlineTicks == 0U) || (frame.ProgrammedDeadlineTicks == MaximumUInt64) ||
        (frame.ObservedTicks < frame.ProgrammedDeadlineTicks) || (frame.ObservedTicks == MaximumUInt64) ||
        (frame.InterruptedInstructionPointer != m_nativePreemptionSentinelEntryPoint) ||
        !Interrupt::IsIa64ExternalPreemptionSentinelRestartInstructionSlot(interruptedInstructionSlot) ||
        ((frame.HardwareInterruptionFunctionState >> 63U) != 0U) ||
        (Interrupt::ValidateIa64ExternalPreemptionCoveredRseState(*m_nativePreemptionControl, frame) !=
         Interrupt::Ia64ExternalPreemptionCoveredRseError::None))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return false;
    }

    const auto active = RunLoop().GetActiveExecutionBinding();
    if (!active.HasValue() || (active.GetValue().ThreadStorage != &m_thread) ||
        (active.GetValue().SchedulingContextStorage != &m_nativePreemptionSchedulingContext) ||
        !active.GetValue().ThreadIdentity.IsEqualTo(m_threadIdentity) ||
        !active.GetValue().SchedulingContextIdentity.IsEqualTo(m_nativePreemptionSchedulingContextIdentity) ||
        (active.GetValue().Processor.GetValue() != m_root.Processor.GetValue()) ||
        (active.GetValue().RunSequence == 0U))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return false;
    }
    frameToken = acquiredFrameToken;
    stoppedRunSequence = active.GetValue().RunSequence;
    return true;
}

Foundation::UInt64 Ia64BspKernelRuntime::CoordinateNativeExternalPreemption() noexcept
{
    if ((m_nativePreemptionControl == nullptr) || (m_state != Ia64BspKernelRuntimeState::Dispatching) ||
        (m_workerPurpose != WorkerPurpose::NativeExternalPreemption))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return 1U;
    }

    Interrupt::Ia64ExternalPreemptionFrame& frame = m_workerContext.GetExternalPreemptionFrame();
    Foundation::UInt64 frameToken{0U};
    Foundation::UInt64 stoppedRunSequence{0U};
    if (!ValidateNativePreemptionCapture(frame, frameToken, stoppedRunSequence))
    {
        return 1U;
    }
    const auto completed = RunLoop().CompleteExternallyStoppedRunAsReady(
        m_thread, m_workerContext,
        Kernel::Scheduler::ProcessorRunExternalStopObservation{
            .Thread = m_threadIdentity,
            .SchedulingContext = m_nativePreemptionSchedulingContextIdentity,
            .RunSequence = stoppedRunSequence,
            .ProgrammedDeadlineTicks = frame.ProgrammedDeadlineTicks,
            .ObservedTicks = frame.ObservedTicks,
            .ExternalFrameToken = frameToken,
            .Processor = m_root.Processor,
            .HardwareState =
                Kernel::Scheduler::ProcessorRunExternalStopHardwareState::TimerMaskedReadBackAndInterruptAcknowledged,
            .Reserved = {},
        });
    if (!completed.HasValue() || (completed.GetValue().Thread.State != Kernel::Scheduler::ThreadState::Ready) ||
        (completed.GetValue().Thread.RunSequence != stoppedRunSequence) ||
        (completed.GetValue().Context.BudgetExhausted != 1U) ||
        (completed.GetValue().Context.RemainingBudgetTicks != 0U))
    {
        if (!completed.HasValue())
        {
            m_dispatchFailure = completed.GetErrorReference();
        }
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return 1U;
    }

    Kernel::Scheduler::SchedulingContextSnapshot scheduling = m_nativePreemptionSchedulingContext.GetSnapshot();
    if ((scheduling.State != Kernel::Scheduler::SchedulingContextState::BudgetExhausted) ||
        !scheduling.BaseThread.IsEqualTo(m_threadIdentity) || !scheduling.CurrentThread.IsEqualTo(m_threadIdentity) ||
        (scheduling.RemainingBudgetTicks != 0U) || (scheduling.NextReplenishmentTicks == 0U) ||
        (scheduling.NextReplenishmentTicks == MaximumUInt64))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return 1U;
    }
    for (;;)
    {
        Platform::Synchronization::ProcessorWaitProgress();
        const Foundation::UInt64 now = m_localTimer.GetTicks();
        if (now == MaximumUInt64)
        {
            m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
            return 1U;
        }
        if (now >= scheduling.NextReplenishmentTicks)
        {
            break;
        }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        __asm__ volatile("hint @pause\n;;\n" ::: "memory");
#endif
    }

    m_workerReceipt = {};
    m_workerFailure = {};
    if (!RunLoop().DispatchRetainedExternalFrameFromCoordinator(m_thread, m_workerContext, m_workerReceipt,
                                                                m_workerFailure) ||
        (m_workerReceipt.ThreadStorage != &m_thread) || (m_workerReceipt.ExternalFrameToken != frameToken) ||
        (m_workerReceipt.ExternalFrameResumeGeneration == 0U) ||
        (m_workerReceipt.StoppedRunSequence != stoppedRunSequence) ||
        (m_workerReceipt.AdmissionRunSequence <= stoppedRunSequence) ||
        (m_workerReceipt.AdmissionDeadlineTicks == 0U) || (m_workerReceipt.AdmissionDeadlineTicks == MaximumUInt64) ||
        (m_workerReceipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
        (m_workerReceipt.ResumeKind != Kernel::Scheduler::ProcessorKernelRunLoopResumeKind::ExternalFrame))
    {
        m_failure.RunLoopError = m_workerFailure.Error;
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return 1U;
    }

    const Time::Ia64LocalTimerSnapshot readmissionTimer = m_localTimer.GetSnapshot();
    if ((readmissionTimer.State != Time::Ia64LocalTimerState::Ready) || (readmissionTimer.Armed != 1U) ||
        (readmissionTimer.ProgrammedDeadlineTicks != m_workerReceipt.AdmissionDeadlineTicks))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return 1U;
    }
    if (!RunLoop().RefreshExternalFrameIdleContinuation(m_workerReceipt, m_workerFailure))
    {
        m_failure.RunLoopError = m_workerFailure.Error;
        m_failure.ExternalPreemptionContinuationError = Ia64ExternalFrameIdleContinuationError::InvalidEntry;
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionContinuationRejected;
        return 1U;
    }
    if (!RunLoop().CommitExternalFrameResume(m_thread, m_workerContext, m_workerReceipt, m_workerFailure))
    {
        m_failure.RunLoopError = m_workerFailure.Error;
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionCoordinatorRejected;
        return 1U;
    }

    m_failure.ExternalPreemptionFrameError = Interrupt::CommitIa64ExternalPreemptionResume(frame, frameToken);
    if (m_failure.ExternalPreemptionFrameError != Interrupt::Ia64ExternalPreemptionFrameError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
        return 1U;
    }

    m_nativePreemptionEvidence = Ia64BspNativePreemptionEvidence{
        .Sequence = 0U,
        .FrameToken = frameToken,
        .ProgrammedDeadlineTicks = frame.ProgrammedDeadlineTicks,
        .ObservedTicks = frame.ObservedTicks,
        .InterruptedInstructionPointer = frame.InterruptedInstructionPointer,
        .InterruptedProcessorStatus = frame.InterruptedProcessorStatus,
        .StoppedRunSequence = stoppedRunSequence,
        .AdmissionRunSequence = m_workerReceipt.AdmissionRunSequence,
        .AdmissionDeadlineTicks = m_workerReceipt.AdmissionDeadlineTicks,
        .IdleContinuationGeneration = m_workerReceipt.ExternalFrameResumeGeneration,
        .CaptureCount = m_nativePreemptionControlSnapshot.CaptureCount,
        .RestoreCount = 0U,
    };
    m_failure.ExternalPreemptionControlError = Interrupt::CommitIa64ExternalPreemptionControlForRestore(
        *m_nativePreemptionControl, frame, frameToken, m_workerReceipt.ExternalFrameResumeGeneration);
    if (m_failure.ExternalPreemptionControlError != Interrupt::Ia64ExternalPreemptionControlError::None)
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
        return 1U;
    }

    Foundation::UInt64 normalizedProcessorStatus{0U};
    m_failure.ExternalPreemptionControlError = Interrupt::PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(
        *m_nativePreemptionControl, frame, normalizedProcessorStatus);
    if ((m_failure.ExternalPreemptionControlError != Interrupt::Ia64ExternalPreemptionControlError::None) ||
        (normalizedProcessorStatus != (frame.InterruptedProcessorStatus & ~InterruptionEnableMask)))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
        return 1U;
    }
    return 0U;
}

void Ia64BspKernelRuntime::RefreshNativePreemptionControlSnapshot() noexcept
{
    m_nativePreemptionControlSnapshot =
        Interrupt::AcquireIa64ExternalPreemptionControlSnapshot(*m_nativePreemptionControl);
}

bool Ia64BspKernelRuntime::ValidateNativePreemptionResume() noexcept
{
    if ((m_nativePreemptionControl == nullptr) || (m_nativePreemptionEvidence.Sequence != 0U) ||
        (m_nativePreemptionEvidence.FrameToken == 0U) || (m_nativePreemptionEvidence.CaptureCount != 1U) ||
        (m_nativePreemptionEvidence.RestoreCount != 0U))
    {
        return false;
    }
    const Interrupt::Ia64ExternalPreemptionFrame& frame = m_workerContext.GetExternalPreemptionFrame();
    RefreshNativePreemptionControlSnapshot();
    const Interrupt::Ia64ExternalPreemptionControlSnapshot& control = m_nativePreemptionControlSnapshot;
    const auto active = RunLoop().GetActiveExecutionBinding();
    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    const Foundation::UInt8 runStopSource =
        static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::RunStop);
    return (control.State == Interrupt::Ia64ExternalPreemptionControlState::Complete) &&
           (control.Mode == Interrupt::Ia64ExternalPreemptionMode::LegacyEvidence) &&
           (control.ArmedDeadlineTicks == 0U) &&
           (control.CapturedFrameToken == m_nativePreemptionEvidence.FrameToken) &&
           (control.IdleContinuationGeneration == m_nativePreemptionEvidence.IdleContinuationGeneration) &&
           (control.CaptureCount == 1U) && (control.RestoreCount == 1U) &&
           !Interrupt::Ia64ExternalPreemptionFrameIsOwned(frame) &&
           (frame.State == Interrupt::Ia64ExternalPreemptionFrameState::Inactive) &&
           (frame.CaptureStage == Interrupt::Ia64ExternalPreemptionCaptureStage::None) && active.HasValue() &&
           (active.GetValue().ThreadStorage == &m_thread) &&
           (active.GetValue().SchedulingContextStorage == &m_nativePreemptionSchedulingContext) &&
           (active.GetValue().RunSequence == m_nativePreemptionEvidence.AdmissionRunSequence) &&
           (controller.State == Kernel::Scheduler::ProcessorRunState::Active) &&
           (controller.Deadline.State == Kernel::Time::ProcessorDeadlineState::Armed) &&
           (controller.Deadline.SelectedSources == runStopSource) &&
           (controller.Deadline.SelectedDeadlineTicks == m_nativePreemptionEvidence.AdmissionDeadlineTicks) &&
           (timer.State == Time::Ia64LocalTimerState::Ready) && (timer.Armed == 1U) &&
           (timer.ProgrammedDeadlineTicks == m_nativePreemptionEvidence.AdmissionDeadlineTicks) &&
           (m_idleContext.GetExternalFrameIdleContinuationGeneration() ==
            m_nativePreemptionEvidence.IdleContinuationGeneration);
}

[[noreturn]] void Ia64BspKernelRuntime::FinishNativePreemptionWorker() noexcept
{
    m_workerReceipt = {};
    m_workerFailure = {};
    static_cast<void>(RunLoop().FinishAndDispatch(m_thread, m_workerContext,
                                                  Kernel::Scheduler::ThreadRunDisposition::Exited,
                                                  Ia64BspNativePreemptionExitCode, m_workerReceipt, m_workerFailure));
    m_failure.RunLoopError = m_workerFailure.Error;
    FailStopKernelRuntime(Diagnostics::Ia64BootStage::NativeWorkerReturn);
}

DispatchResult Ia64BspKernelRuntime::DispatchNativeExternalPreemptionProbe() noexcept
{
#if !defined(ASTERIA_TARGET_IA64) || !defined(__ia64__)
    return DispatchResult::Failure(Ia64BspKernelRuntimeError::NativePreemptionUnavailable);
#else
    if ((m_state == Ia64BspKernelRuntimeState::Dispatching) || (m_state == Ia64BspKernelRuntimeState::Complete) ||
        (m_state == Ia64BspKernelRuntimeState::Failed))
    {
        return DispatchResult::Failure(Ia64BspKernelRuntimeError::NativePreemptionAlreadyAttempted);
    }
    if (m_state != Ia64BspKernelRuntimeState::Prepared)
    {
        return DispatchResult::Failure(Ia64BspKernelRuntimeError::NotPrepared);
    }

    const Kernel::Scheduler::ProcessorRunSnapshot controller = Controller().GetSnapshot();
    const Kernel::Scheduler::ProcessorKernelRunLoopSnapshot runLoop = RunLoop().GetSnapshot();
    const Kernel::Scheduler::ThreadSnapshot thread = m_thread.GetSnapshot();
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    if ((m_runtimeMemory == nullptr) || (m_resourceDomain == nullptr) || (m_issuer == nullptr) ||
        (m_localTimerProbeState != Ia64BspKernelRuntimeLocalTimerProbeState::Complete) || !m_root.Processor.IsValid() ||
        (m_root.Processor.GetValue() != 0U) || (m_timerVector != Interrupt::Ia64ExternalInterruptTimerVector) ||
        (m_workerPurpose != WorkerPurpose::None) || (thread.State != Kernel::Scheduler::ThreadState::Dormant) ||
        (controller.State != Kernel::Scheduler::ProcessorRunState::Idle) ||
        (controller.Mode != Kernel::Scheduler::ProcessorRunDispatchMode::HardwareDeadline) ||
        (controller.Scheduler.Queue.ReadyEntries != 0U) || (controller.Scheduler.Queue.ClaimedEntries != 0U) ||
        (controller.Scheduler.Queue.ReservedEntries != 0U) || (controller.TimerQueue.ScheduledEntries != 0U) ||
        (controller.TimerQueue.PendingEntries != 0U) || (controller.TimerQueue.ClaimedEntries != 0U) ||
        (controller.Deadline.State != Kernel::Time::ProcessorDeadlineState::Disarmed) ||
        (runLoop.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle) || (runLoop.BindingCount != 0U) ||
        (runLoop.ExternalFrameRetained != 0U) || (runLoop.ExternalFrameResumePending != 0U) ||
        (timer.State != Time::Ia64LocalTimerState::Ready) || (timer.Armed != 0U) ||
        (timer.ProgrammedDeadlineTicks != 0U) || !m_workerContext.IsPrepared() ||
        Interrupt::Ia64ExternalPreemptionFrameIsOwned(m_workerContext.GetExternalPreemptionFrame()))
    {
        return FailDispatch(Ia64BspKernelRuntimeError::NativePreemptionPrerequisiteRejected);
    }

    if (!PrepareNativePreemptionCorridor())
    {
        return FailDispatch(m_failure.Error);
    }
    // Corridor validation may inspect every locked translation before native
    // admission. Anchor the finite scheduling window only after that work so
    // its absolute deadline measures runnable execution, not preparation.
    if (!InitializeNativePreemptionSchedulingContext())
    {
        return FailDispatch(m_failure.Error);
    }

    m_nativePreemptionEvidence = {};
    m_nativePreemptionControlSnapshot = {};
    m_state = Ia64BspKernelRuntimeState::Dispatching;
    m_workerPurpose = WorkerPurpose::NativeExternalPreemption;
    m_failure.ThreadError = m_thread.MakeReady();
    if (m_failure.ThreadError != Kernel::Scheduler::ThreadError::None)
    {
        return FailDispatch(Ia64BspKernelRuntimeError::ThreadReadyRejected);
    }
    if (!BindWorker())
    {
        return FailDispatch(Ia64BspKernelRuntimeError::RunLoopBindingRejected);
    }

    m_dispatchFailure = {};
    if (!RunLoop().EnqueueBoundThread(m_nativePreemptionSchedulingContextIdentity, m_nativePreemptionSchedulingContext,
                                      m_threadIdentity, m_thread, m_dispatchFailure))
    {
        m_failure.RunLoopError = m_dispatchFailure.Error;
        m_failure.ControllerError = m_dispatchFailure.ControllerError;
        m_failure.SchedulerError = m_dispatchFailure.SchedulerError;
        m_failure.DeadlineError = m_dispatchFailure.DeadlineError;
        return FailDispatch(Ia64BspKernelRuntimeError::EnqueueRejected);
    }

    // Accepted native execution never returns to this cooperative idle frame.
    // Entry replaces it with a fresh dedicated landing before restoring the
    // worker. Any return here is therefore a terminal corridor contradiction.
    static_cast<void>(DispatchWorkerFromIdle());
    return FailDispatch(Ia64BspKernelRuntimeError::DispatchResultRejected);
#endif
}

[[noreturn]] void Ia64BspKernelRuntime::LandNativeExternalPreemptionIdle() noexcept
{
    if ((m_nativePreemptionControl == nullptr) || (m_state != Ia64BspKernelRuntimeState::Dispatching) ||
        (m_workerPurpose != WorkerPurpose::NativeExternalPreemption))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::NativeIdleState);
    }
    m_workerReceipt = {};
    m_workerFailure = {};
    const bool idleResumed = RunLoop().FinishSelectedFreshIdleLanding(m_workerReceipt, m_workerFailure);
    // The worker exited; which exit code it reported is recorded below,
    // not compared.
    if (idleResumed || !ValidateExpectedDispatchFailure(m_workerFailure) ||
        !ValidateDispatchOwnerState(m_thread.GetSnapshot().ExitCode))
    {
        m_failure.RunLoopError = m_workerFailure.Error;
        m_failure.ControllerError = m_workerFailure.ControllerError;
        m_failure.SchedulerError = m_workerFailure.SchedulerError;
        m_failure.DeadlineError = m_workerFailure.DeadlineError;
        m_failure.AddressSpaceControllerError = m_workerFailure.AddressSpaceController.Error;
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::NativeIdleDispatch);
    }
    const Interrupt::Ia64ExternalPreemptionControlSnapshot control =
        Interrupt::AcquireIa64ExternalPreemptionControlSnapshot(*m_nativePreemptionControl);
    if ((control.State != Interrupt::Ia64ExternalPreemptionControlState::Complete) ||
        (control.Mode != Interrupt::Ia64ExternalPreemptionMode::LegacyEvidence) ||
        (control.CapturedFrameToken != m_nativePreemptionEvidence.FrameToken) ||
        (control.IdleContinuationGeneration != m_nativePreemptionEvidence.IdleContinuationGeneration) ||
        (m_nativePreemptionEvidence.Sequence != 0U))
    {
        m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::NativeIdleControl);
    }

    // The proof records what happened (captures, restores, the worker's exit
    // code); the host judges it. Only the retained evidence's own
    // publication and the halt remain here.
    m_nativePreemptionEvidence.RestoreCount = control.RestoreCount;
    m_state = Ia64BspKernelRuntimeState::Complete;
    Platform::Synchronization::AtomicUInt64Reference sequence{m_nativePreemptionEvidence.Sequence};
    sequence.StoreRelease(1U);
    Platform::Ia64::Diagnostics::Ia64NativePreemptionEvidence& evidence =
        Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().NativePreemption;
    evidence.Runtime = m_nativePreemptionEvidence;
    evidence.ControlCaptureCount = control.CaptureCount;
    evidence.ControlRestoreCount = control.RestoreCount;
    evidence.WorkerExitCode = m_thread.GetSnapshot().ExitCode;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:STAGE:NATIVE-PREEMPTION-RECORDED\r\n");
    Platform::Firmware::HaltAfterFirmwareExit();
}
#endif

DispatchResult Ia64BspKernelRuntime::DispatchBootstrapWorker() noexcept
{
    if (m_state == Ia64BspKernelRuntimeState::Dispatching || m_state == Ia64BspKernelRuntimeState::Complete)
    {
        return DispatchResult::Failure(Ia64BspKernelRuntimeError::DispatchAlreadyAttempted);
    }
    if (m_state != Ia64BspKernelRuntimeState::Prepared)
    {
        return DispatchResult::Failure(Ia64BspKernelRuntimeError::NotPrepared);
    }
    m_state = Ia64BspKernelRuntimeState::Dispatching;
    m_workerPurpose = WorkerPurpose::BootstrapRoundTrip;

    m_failure.ThreadError = m_thread.MakeReady();
    if (m_failure.ThreadError != Kernel::Scheduler::ThreadError::None)
    {
        return FailDispatch(Ia64BspKernelRuntimeError::ThreadReadyRejected);
    }
    if (!BindWorker())
    {
        return FailDispatch(Ia64BspKernelRuntimeError::RunLoopBindingRejected);
    }
    if (!EnqueueWorker())
    {
        return FailDispatch(Ia64BspKernelRuntimeError::EnqueueRejected);
    }

    const bool dispatch = DispatchWorkerFromIdle();
    if (dispatch || !ValidateExpectedDispatchFailure(m_dispatchFailure))
    {
        m_failure.RunLoopError = m_dispatchFailure.Error;
        m_failure.ControllerError = m_dispatchFailure.ControllerError;
        m_failure.SchedulerError = m_dispatchFailure.SchedulerError;
        m_failure.DeadlineError = m_dispatchFailure.DeadlineError;
        m_failure.AddressSpaceControllerError = m_dispatchFailure.AddressSpaceController.Error;
        return FailDispatch(Ia64BspKernelRuntimeError::DispatchResultRejected);
    }
    if (!ValidateDispatchOwnerState(Ia64BspKernelWorkerExitCode) || !ValidateWorkerEvidence())
    {
        return FailDispatch(Ia64BspKernelRuntimeError::DispatchPostconditionRejected);
    }

    m_state = Ia64BspKernelRuntimeState::Complete;
    return DispatchResult::Success(Ia64BspKernelRuntimeDispatchSummary{
        .ExitCode = Ia64BspKernelWorkerExitCode,
        .Processor = m_root.Processor,
        .State = m_state,
        .Reserved = {},
    });
}

UserProbeDispatchResult Ia64BspKernelRuntime::DispatchUserExecutionProbe() noexcept
{
    if ((m_userProbeState == Ia64BspKernelRuntimeUserProbeState::Dispatching) ||
        (m_userProbeState == Ia64BspKernelRuntimeUserProbeState::DispatchFailed))
    {
        return UserProbeDispatchResult::Failure(Ia64BspKernelRuntimeUserProbeError::DispatchAlreadyAttempted);
    }
    if ((m_state != Ia64BspKernelRuntimeState::Prepared) ||
        (m_userProbeState != Ia64BspKernelRuntimeUserProbeState::Prepared))
    {
        return UserProbeDispatchResult::Failure(Ia64BspKernelRuntimeUserProbeError::DispatchNotPrepared);
    }

    m_state = Ia64BspKernelRuntimeState::Dispatching;
    m_userProbeState = Ia64BspKernelRuntimeUserProbeState::Dispatching;
    m_workerPurpose = WorkerPurpose::UserExecutionProbe;
    m_userProbeFailure.Thread = m_thread.MakeReady();
    m_failure.ThreadError = m_userProbeFailure.Thread;
    if (m_userProbeFailure.Thread != Kernel::Scheduler::ThreadError::None)
    {
        return FailUserProbeDispatch(Ia64BspKernelRuntimeUserProbeError::ThreadReadyRejected);
    }
    if (!BindWorker())
    {
        m_userProbeFailure.RunLoop = m_failure.RunLoopError;
        return FailUserProbeDispatch(Ia64BspKernelRuntimeUserProbeError::RunLoopBindingRejected);
    }
    if (!EnqueueWorker())
    {
        m_userProbeFailure.Controller = m_failure.ControllerError;
        m_userProbeFailure.Scheduler = m_failure.SchedulerError;
        m_userProbeFailure.Deadline = m_failure.DeadlineError;
        m_userProbeFailure.AddressSpaceController = m_failure.AddressSpaceControllerError;
        return FailUserProbeDispatch(Ia64BspKernelRuntimeUserProbeError::EnqueueRejected);
    }

    const bool dispatch = DispatchWorkerFromIdle();
    if (!dispatch && ValidateExpectedDispatchFailure(m_dispatchFailure) && ValidateUserProbeDispatchOwnerState())
    {
        RetainUserProbeDispatchFailure();
        m_state = Ia64BspKernelRuntimeState::Failed;
        return UserProbeDispatchResult::Failure(Ia64BspKernelRuntimeUserProbeError::LaunchRejected);
    }
    if (dispatch)
    {
        RetainUserProbeDispatchFailure();
        return FailUserProbeDispatch(Ia64BspKernelRuntimeUserProbeError::DispatchResultRejected);
    }
    RetainUserProbeDispatchFailure();
    if (!CompleteUserProbeWaitFromIdle())
    {
        return FailUserProbeDispatch(m_userProbeFailure.Error);
    }

    // Each re-admission restores the retained syscall and enters CPL3. This
    // frame becomes runnable again only if that user thread prepares another
    // WaitSet wait and restores the worker's repeated supervisor continuation.
    for (;;)
    {
        Platform::Synchronization::ProcessorWaitProgress();
        if (DispatchWorkerFromIdle())
        {
            return FailUserProbeDispatch(Ia64BspKernelRuntimeUserProbeError::WaitResumeRejected);
        }
        RetainUserProbeDispatchFailure();
        if (!CompleteUserProbeWaitFromIdle())
        {
            return FailUserProbeDispatch(m_userProbeFailure.Error);
        }
    }
}

void Ia64BspKernelRuntime::BindShootdownMailbox(Kernel::Memory::TlbShootdownMailboxTransport& mailbox) noexcept
{
    m_initiatorDrain = ::new (static_cast<void*>(m_initiatorDrainStorage))
        Kernel::Memory::TlbShootdownDrain{mailbox, *m_tlbShootdownDispatcher};
    Platform::Synchronization::BindProcessorDrain(m_initiatorDrain, nullptr);
    // BSP identity and runtime IVT are installed before this binding.
    Diagnostics::Ia64StopProcessorOnline();
    if (!Platform::Synchronization::SynchronizeOnlineProcessorClock(m_localTimer, 0U,
                                                                    Diagnostics::GetIa64EvidenceRecord().Clocks))
    {
        Diagnostics::StopIa64(Kernel::Diagnostics::StopCode::InvariantViolated);
    }
    if (mailbox.GetOnlineProcessorMask() == 1U)
    {
        mailbox.BindProcessorSet(Platform::Synchronization::RuntimeProcessorSet());
    }
}

System::Ia64PagerCollaborators Ia64BspKernelRuntime::GetPagerCollaborators() noexcept
{
    return System::Ia64PagerCollaborators{
        .Registry = &m_objectRegistry,
        .Issuer = m_issuer,
        .Revocations = &m_revocationRegistry,
        .Clock = &m_localTimer,
        .Controller = m_controller,
        .RunLoop = m_runLoop,
        .Shootdown = m_tlbShootdownDispatcher,
        .RuntimeMemory = m_runtimeMemory,
        .Resources = m_resourceDomain,
        .ResourceIdentity = m_resourceDomainIdentity,
        .ClientSpace = m_userRoot.AddressSpaceIdentity,
        .Diagnostic = m_initialUserImageDiagnosticChannelIdentity,
        .Scratch = &m_systemCallScratch,
        .Preemption = this,
        .Processor = m_root.Processor,
        .ImplementedVirtualAddressMostSignificantBit =
            m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit,
    };
}

// The pager serves the initial thread's faults: the thread blocks on its
// bound worker context, which after the one-way launch runs on the thread's
// entry storage.
bool Ia64BspKernelRuntime::AttachPager(System::Ia64PagerRuntime& pager) noexcept
{
    if ((m_pager != nullptr) || !pager.AttachClient(System::Ia64PagerClient{
                                    .Thread = &m_thread,
                                    .Context = &m_workerContext,
                                    .ThreadIdentity = m_threadIdentity,
                                    .SchedulingContextIdentity = m_schedulingContextIdentity,
                                    .ProtectionDomainIdentity = m_protectionDomainIdentity.GetValue(),
                                }))
    {
        return false;
    }
    m_pager = &pager;
    return true;
}

System::Ia64UserThreadTableCollaborators Ia64BspKernelRuntime::GetThreadTableCollaborators() noexcept
{
    return System::Ia64UserThreadTableCollaborators{
        .RunLoop = m_runLoop,
        .RuntimeMemory = m_runtimeMemory,
        .Clock = &m_localTimer,
        .Scratch = &m_systemCallScratch,
        .Preemption = this,
        .Processor = m_root.Processor,
        .ImplementedVirtualAddressMostSignificantBit =
            m_palVirtualMemoryCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit,
        .Registry = &m_objectRegistry,
        .Wake = &m_controller->GetWakeFacade(),
        .WaitClock = &m_localTimer,
    };
}

bool Ia64BspKernelRuntime::AttachThreads(System::Ia64UserThreadTable& threads) noexcept
{
    if ((m_threads != nullptr) || (m_pager == nullptr))
    {
        return false;
    }
    m_threads = &threads;
    return true;
}

bool Ia64BspKernelRuntime::AttachDeviceInterrupts(Interrupt::Ia64InterruptLines& lines) noexcept
{
    if ((m_deviceInterrupts != nullptr) || (m_pager == nullptr))
    {
        return false;
    }
    m_deviceInterrupts = &lines;
    return true;
}

bool Ia64BspKernelRuntime::PreemptUser(const Memory::Ia64BspRuntimeUserDomain domain) noexcept
{
    return PreemptScheduledUser(Memory::Ia64BspRuntimeEntrySlotOf(domain));
}

bool Ia64BspKernelRuntime::PreemptThread(const Foundation::Size slot) noexcept
{
    return (m_threads != nullptr) && (slot < Memory::Ia64BspRuntimeThreadEntrySlots) &&
           PreemptScheduledUser(Memory::Ia64BspRuntimeThreadEntrySlot(slot));
}

bool Ia64BspKernelRuntime::YieldUser(const Memory::Ia64BspRuntimeUserDomain domain) noexcept
{
    const Memory::Ia64BspRuntimeEntrySlot slot = Memory::Ia64BspRuntimeEntrySlotOf(domain);
    return FinishUserRun(slot) && m_runtimeMemory->SelectEntryContexts(m_root.Processor, slot);
}

bool Ia64BspKernelRuntime::PreemptScheduledUser(const Memory::Ia64BspRuntimeEntrySlot slot) noexcept
{
    // The claim masked the timer for the due deadline the local timer still
    // records; the deadline controller keeps it until a refresh or the next
    // dispatch programs another one, which unmasks the timer again.
    const Time::Ia64LocalTimerSnapshot timer = m_localTimer.GetSnapshot();
    if ((timer.Armed == 0U) || (m_localTimer.AcknowledgeExternallyMaskedDeadline(timer.ProgrammedDeadlineTicks) !=
                                Hal::Time::LocalTimerError::None))
    {
        return RejectUserTick(1U);
    }
    Platform::Synchronization::ProcessorWatchdogTick(
        m_localTimer.GetTicks(), reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0)));
    const auto entry = static_cast<Foundation::Size>(slot);
    const auto first = static_cast<Foundation::Size>(Memory::Ia64BspRuntimeEntrySlot::Thread0);
    const auto thread = entry >= first ? entry - first : Kernel::Scheduler::MaximumReadyQueueEntries;
    Kernel::Diagnostics::RecordUserProcessor(Diagnostics::GetIa64EvidenceRecord().UserProcessors, 0U, thread, false,
                                             true);
    ++m_userTicks;
    auto& evidence = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().UserPreemption;
    evidence.UserTicks = m_userTicks;
    Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
    // A tick is a scheduling point like idle: with a CPU-bound thread the processor may never idle.
    // Device vectors pending at it are dispatched first, so a woken wait wins over its own
    // deadline: one that became pending while PSR.i was clear may still be pending when the tick
    // comes due, and the tick outranks it at cr.ivr (an emulator that gives other processors a time
    // slice before the rfi that unmasks it makes that the rule;
    // Documentation/Architecture/IommuDomain.md ).
    static_cast<void>(PollDeviceInterruptsFromIdle());
    if ((m_pager != nullptr) && (m_pager->ProgressFromIdle() == System::Ia64PagerIdleProgress::Failed))
    {
        return RejectUserTick(2U);
    }
    const int fired = RefreshUserRunStop();
    if (fired < 0)
    {
        return RejectUserTick(3U);
    }
    if (fired > 0)
    {
        // A boot domain's slot index is its domain index; created threads'
        // stops are counted apart.
        if (slot < Memory::Ia64BspRuntimeEntrySlot::Thread0)
        {
            ++evidence.RunStops[static_cast<Foundation::Size>(slot)];
        }
        else
        {
            auto& threads = Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().Threads;
            threads.RunStops = ++m_threadRunStops;
            Platform::Ia64::Diagnostics::PublishIa64Evidence(threads.Published);
        }
        Platform::Ia64::Diagnostics::PublishIa64Evidence(evidence.Published);
        if (!FinishUserRun(slot))
        {
            return RejectUserTick(4U);
        }
    }
    return m_runtimeMemory->SelectEntryContexts(m_root.Processor, slot) || RejectUserTick(5U);
}

int Ia64BspKernelRuntime::RefreshUserRunStop() noexcept
{
    const auto refreshed = Controller().RefreshTimerQueueDeadline(m_localTimer.GetTicks());
    if (refreshed.HasValue())
    {
        return 0;
    }
    return refreshed.GetErrorReference().Error == Kernel::Scheduler::ProcessorRunError::DeadlineReached ? 1 : -1;
}

bool Ia64BspKernelRuntime::FinishUserRun(const Memory::Ia64BspRuntimeEntrySlot slot) noexcept
{
    if (slot == Memory::Ia64BspRuntimeEntrySlot::Pager)
    {
        return (m_pager != nullptr) && m_pager->Preempt();
    }
    if (slot != Memory::Ia64BspRuntimeEntrySlot::InitialComponent)
    {
        return (m_threads != nullptr) &&
               m_threads->Preempt(static_cast<Foundation::Size>(slot) -
                                  static_cast<Foundation::Size>(Memory::Ia64BspRuntimeEntrySlot::Thread0));
    }
    return (RunLoop().GetSnapshot().ActiveThread == &m_thread) &&
           RunLoop().FinishAndDispatch(m_thread, m_workerContext, Kernel::Scheduler::ThreadRunDisposition::Ready, 0U,
                                       m_userPreemptReceipt, m_userPreemptFailure) &&
           (m_userPreemptReceipt.ThreadStorage == &m_thread) &&
           (m_userPreemptReceipt.State == Kernel::Scheduler::ProcessorKernelRunLoopState::Active);
}

bool Ia64BspKernelRuntime::RejectUserTick(const Foundation::UInt8 stage) noexcept
{
    const char text[]{'0', 'x', "0123456789ABCDEF"[stage >> 4U], "0123456789ABCDEF"[stage & 15U], '\0'};
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-USER-TICK:FAIL:");
    Platform::Firmware::WriteDiagnosticAscii(text);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
    return false;
}

void Ia64BspKernelRuntime::ReportDispatchFailure(
    const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    const Foundation::UInt64 detail = (static_cast<Foundation::UInt64>(failure.Error) << 40U) |
                                      (static_cast<Foundation::UInt64>(failure.ControllerError) << 32U) |
                                      (static_cast<Foundation::UInt64>(failure.SchedulerError) << 24U) |
                                      (static_cast<Foundation::UInt64>(failure.DeadlineReceipt.State) << 16U) |
                                      (static_cast<Foundation::UInt64>(failure.DeadlineReceipt.SelectedSources) << 8U) |
                                      static_cast<Foundation::UInt64>(failure.ActiveRunRetained);
    char text[19U]{'0', 'x'};
    for (Foundation::Size index{0U}; index < 16U; ++index)
    {
        text[index + 2U] = "0123456789ABCDEF"[(detail >> ((15U - index) * 4U)) & 15U];
    }
    Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-USER-DISPATCH:REJECTED:");
    Platform::Firmware::WriteDiagnosticAscii(text);
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
}

bool Ia64BspKernelRuntime::IsReplenishmentWait(const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    constexpr auto retry = static_cast<Foundation::UInt8>(Kernel::Time::ProcessorDeadlineSource::SchedulerRetry);
    return (failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected) &&
           (failure.ControllerError == Kernel::Scheduler::ProcessorRunError::NoRunnableThread) &&
           (failure.DeadlineReceipt.State == Kernel::Time::ProcessorDeadlineState::Armed) &&
           ((failure.DeadlineReceipt.SelectedSources & retry) != 0U) &&
           (failure.DeadlineReceipt.SelectedDeadlineTicks != 0U) && (failure.ActiveRunRetained == 0U);
}

bool Ia64BspKernelRuntime::WaitForReplenishmentFromIdle(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (deadlineTicks > MaximumUInt64 - ReplenishmentWaitGraceTicks)
    {
        return false;
    }
    // The pager's wait deadline is bounded by the ABI; only a replenishment
    // deadline beyond the grace is wrong.
    const Foundation::UInt64 waitDeadline = (m_pager != nullptr) ? m_pager->GetWaitDeadline() : 0U;
    const bool waitFirst = (waitDeadline != 0U) && (waitDeadline < deadlineTicks);
    const Foundation::UInt64 target = waitFirst ? waitDeadline : deadlineTicks;
    for (;;)
    {
        Platform::Synchronization::ProcessorWaitProgress();
        const Foundation::UInt64 now = m_localTimer.GetTicks();
        if (now >= target)
        {
            return true;
        }
        if (!waitFirst && (now + ReplenishmentWaitGraceTicks < target))
        {
            return false;
        }
        // An Interrupt it signalled may have woken a thread to dispatch.
        if (PollDeviceInterruptsFromIdle())
        {
            return true;
        }
    }
}

Ia64BspKernelRuntime::IdleWait Ia64BspKernelRuntime::WaitForPagerDeadlineFromIdle() noexcept
{
    const Foundation::UInt64 deadline = (m_pager != nullptr) ? m_pager->GetWaitDeadline() : 0U;
    if (deadline != 0U)
    {
        while (m_localTimer.GetTicks() < deadline)
        {
            Platform::Synchronization::ProcessorWaitProgress();
            if (PollDeviceInterruptsFromIdle())
            {
                return IdleWait::DeviceInterrupt;
            }
        }
        return IdleWait::Deadline;
    }
    // A driver blocked on its Interrupt with nothing else to run waits as
    // long as its device takes.
    if ((m_deviceInterrupts == nullptr) || !m_deviceInterrupts->HasBoundLines())
    {
        return IdleWait::None;
    }
    while (!PollDeviceInterruptsFromIdle())
    {
    }
    return IdleWait::DeviceInterrupt;
}

bool Ia64BspKernelRuntime::PollDeviceInterruptsFromIdle() noexcept
{
    Diagnostics::PollIa64Stop();
    if (m_deviceInterrupts == nullptr)
    {
        return false;
    }
    const Interrupt::Ia64DeviceInterruptPoll polled = Interrupt::DeliverPendingDeviceInterrupts(*m_deviceInterrupts);
    if (polled == Interrupt::Ia64DeviceInterruptPoll::Unexpected)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-INTERRUPT:FAIL:UNEXPECTED-VECTOR\r\n");
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::DeviceInterrupt);
    }
    return polled == Interrupt::Ia64DeviceInterruptPoll::Delivered;
}

bool Ia64BspKernelRuntime::IsLateDispatch(const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    // An admission whose run stop passed before entry rolls back to Ready; a
    // deadline due before it could be programmed is nonterminal too
    // (ProcessorRunController.md). Either is retried.
    return (failure.ControllerError == Kernel::Scheduler::ProcessorRunError::DeadlineReached) &&
           (((failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerEntryRejected) &&
             (failure.ActiveRunRetained == 0U)) ||
            (failure.Error == Kernel::Scheduler::ProcessorKernelRunLoopError::ControllerRejected));
}

InitialUserImageDispatchResult Ia64BspKernelRuntime::DispatchInitialUserImage() noexcept
{
    if ((m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::Dispatching) ||
        (m_initialUserImageState == Ia64BspKernelRuntimeInitialUserImageState::DispatchFailed))
    {
        return InitialUserImageDispatchResult::Failure(
            Ia64BspKernelRuntimeInitialUserImageError::DispatchAlreadyAttempted);
    }
    if ((m_state != Ia64BspKernelRuntimeState::Prepared) ||
        (m_initialUserImageState != Ia64BspKernelRuntimeInitialUserImageState::Prepared))
    {
        return InitialUserImageDispatchResult::Failure(Ia64BspKernelRuntimeInitialUserImageError::DispatchNotPrepared);
    }

    m_state = Ia64BspKernelRuntimeState::Dispatching;
    m_initialUserImageState = Ia64BspKernelRuntimeInitialUserImageState::Dispatching;
    m_workerPurpose = WorkerPurpose::InitialUserImage;
    // The boot pager is queued first, so it waits for faults before the
    // initial image runs.
    if ((m_pager != nullptr) && !m_pager->Admit(m_dispatchFailure))
    {
        RetainInitialUserImageDispatchFailure();
        return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::EnqueueRejected);
    }
    if (m_pager != nullptr)
    {
        // The boot threads run in time slices from here.
        Platform::Ia64::Diagnostics::PublishIa64Evidence(
            Platform::Ia64::Diagnostics::GetIa64EvidenceRecord().UserPreemption.Published);
    }
    m_initialUserImageFailure.Thread = m_thread.MakeReady();
    m_failure.ThreadError = m_initialUserImageFailure.Thread;
    if (m_initialUserImageFailure.Thread != Kernel::Scheduler::ThreadError::None)
    {
        return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::ThreadReadyRejected);
    }
    if (!BindWorker())
    {
        m_initialUserImageFailure.RunLoop = m_failure.RunLoopError;
        return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::RunLoopBindingRejected);
    }
    if (!EnqueueWorker())
    {
        RetainInitialUserImageDispatchFailure();
        return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::EnqueueRejected);
    }

    bool dispatch = DispatchWorkerFromIdle();
    RetainInitialUserImageDispatchFailure();
    // With the boot pager, the CPU returns here whenever both threads are
    // blocked: a fault the initial thread delivered wakes the pager. An exited
    // initial thread leaves the pager and created threads running. A late
    // deadline is retried, bounded while no PL3 tick shows that a thread ran.
    Foundation::Size late{0U};
    Foundation::UInt64 ticks{m_userTicks};
    while (m_pager != nullptr)
    {
        Platform::Synchronization::ProcessorWaitProgress();
        // Exited created threads are unbound at every scheduling point the idle loop reaches.
        if ((m_threads != nullptr) && !m_threads->Reap())
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-THREAD:FAIL:REAP\r\n");
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::Reap);
        }
        if (!dispatch && IsLateDispatch(m_dispatchFailure))
        {
            if (m_userTicks != ticks)
            {
                ticks = m_userTicks;
                late = 0U;
            }
            if (++late > LateDispatchAttempts)
            {
                Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-PAGER:FAIL:DISPATCH-LATE\r\n");
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::DispatchLate);
            }
            dispatch = DispatchWorkerFromIdle();
            RetainInitialUserImageDispatchFailure();
            continue;
        }
        late = 0U;
        // Every runnable thread has spent its budget until a replenishment:
        // a fault the pager must serve still wakes it first; otherwise idle
        // polls the ITC up to the armed deadline, since a PL0 tick cannot
        // reach it (the tick stays pending and arrives at the next PL3 entry,
        // where the claim finds it not due).
        if (!dispatch && IsReplenishmentWait(m_dispatchFailure))
        {
            const System::Ia64PagerIdleProgress pager = m_pager->ProgressFromIdle();
            if ((pager == System::Ia64PagerIdleProgress::Failed) ||
                ((pager == System::Ia64PagerIdleProgress::Waiting) &&
                 !WaitForReplenishmentFromIdle(m_dispatchFailure.DeadlineReceipt.SelectedDeadlineTicks)))
            {
                Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-PAGER:FAIL:REPLENISHMENT\r\n");
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::Replenishment);
            }
            dispatch = DispatchWorkerFromIdle();
            RetainInitialUserImageDispatchFailure();
            continue;
        }
        if (dispatch || !ValidateExpectedDispatchFailure(m_dispatchFailure))
        {
            ReportDispatchFailure(m_dispatchFailure);
            return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::DispatchResultRejected);
        }
        // Nothing runnable and no replenishment armed: a fault or an exit has woken the pager, or
        // its timed wait will, so idle polls the ITC to that deadline. A device interrupt that woke
        // nobody leaves it to dispatch again.
        System::Ia64PagerIdleProgress idlePager = m_pager->ProgressFromIdle();
        const IdleWait waited =
            (idlePager == System::Ia64PagerIdleProgress::Waiting) ? WaitForPagerDeadlineFromIdle() : IdleWait::None;
        if (waited != IdleWait::None)
        {
            idlePager = m_pager->ProgressFromIdle();
        }
        if ((idlePager == System::Ia64PagerIdleProgress::Failed) ||
            ((idlePager != System::Ia64PagerIdleProgress::Woken) && (waited != IdleWait::DeviceInterrupt)))
        {
            Platform::Firmware::WriteDiagnosticAscii("ASTERIA:IA64-PAGER:IDLE-STALL\r\n");
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::IdleStall);
        }
        dispatch = DispatchWorkerFromIdle();
        RetainInitialUserImageDispatchFailure();
    }
    if (dispatch || !ValidateExpectedDispatchFailure(m_dispatchFailure))
    {
        return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::DispatchResultRejected);
    }
    if (!ValidateInitialUserImageDispatchOwnerState())
    {
        return FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError::DispatchPostconditionRejected);
    }
    m_state = Ia64BspKernelRuntimeState::Failed;
    return InitialUserImageDispatchResult::Failure(Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected);
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void Ia64BspKernelRuntime::WorkerEntry(void* const argument) noexcept
{
    if (argument == nullptr)
    {
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::WorkerArgument);
    }
    auto* const owner = static_cast<Ia64BspKernelRuntime*>(argument);
    owner->RunWorker();
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void Ia64BspKernelRuntime::RunInitialUserImageWorker() noexcept
{
    if (!ValidateBorrowedInitialComponentRoot() ||
        (m_root.ControllerStorage->GetSnapshot().ActiveStorage != m_userRoot.AddressSpaceStorage) ||
        (m_initialUserImageLaunchOwner.GetState() != Ia64InitialUserImageLaunchOwnerState::Ready))
    {
        m_initialUserImageFailure.Error = Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::InitialRoot);
    }
    // The pager may have entered PL3 first and selected its own entry
    // contexts.
    if ((m_runtimeMemory == nullptr) ||
        !m_runtimeMemory->SelectUserEntryContexts(m_root.Processor, Memory::Ia64BspRuntimeUserDomain::InitialComponent))
    {
        m_initialUserImageFailure.Error = Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected;
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::InitialRoot);
    }
    m_initialUserImageFailure.Launch = Ia64UserModeLaunch::Launch(m_initialUserImageLaunchRequest);
    m_initialUserImageFailure.Error = Ia64BspKernelRuntimeInitialUserImageError::LaunchRejected;
    m_initialUserImageFailure.ExitCode = Ia64BspInitialUserImageLaunchFailureExitCode;
    m_initialUserImageState = Ia64BspKernelRuntimeInitialUserImageState::DispatchFailed;
    if (!IsRecoverableUserProbeLaunchError(m_initialUserImageFailure.Launch))
    {
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::InitialLaunch);
    }
    static_cast<void>(
        RunLoop().FinishAndDispatch(m_thread, m_workerContext, Kernel::Scheduler::ThreadRunDisposition::Exited,
                                    Ia64BspInitialUserImageLaunchFailureExitCode, m_workerReceipt, m_workerFailure));
    FailStopKernelRuntime(Diagnostics::Ia64BootStage::InitialWorkerReturn);
}

[[noreturn]] ASTERIA_NOINLINE_NOIPA void Ia64BspKernelRuntime::RunWorker() noexcept
{
#if ASTERIA_IA64_BSP_NATIVE_PREEMPTION_PROBE_CODE
    if (m_workerPurpose == WorkerPurpose::NativeExternalPreemption)
    {
        if (!ArmNativePreemptionSentinel() || !ValidateNativePreemptionResume())
        {
            m_failure.Error = Ia64BspKernelRuntimeError::NativePreemptionRestoreRejected;
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::NativeIdleState);
        }
        FinishNativePreemptionWorker();
    }
#endif
    if (m_workerPurpose == WorkerPurpose::InitialUserImage)
    {
        RunInitialUserImageWorker();
    }
    if (m_workerPurpose == WorkerPurpose::UserExecutionProbe)
    {
        if (!ValidateBorrowedInitialComponentRoot() ||
            (m_root.ControllerStorage->GetSnapshot().ActiveStorage != m_userRoot.AddressSpaceStorage))
        {
            m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::LaunchRejected;
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeRoot);
        }
        const Ia64UserModeLaunchResult launched = Ia64UserModeLaunch::LaunchResumable(m_userProbeLaunchRequest);
        if (!launched.HasValue())
        {
            m_userProbeFailure.Launch = launched.GetError();
            m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::LaunchRejected;
            m_userProbeFailure.ExitCode = Ia64BspUserProbeLaunchFailureExitCode;
            m_userProbeState = Ia64BspKernelRuntimeUserProbeState::DispatchFailed;
            if (!IsRecoverableUserProbeLaunchError(m_userProbeFailure.Launch))
            {
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeLaunch);
            }
            static_cast<void>(
                RunLoop().FinishAndDispatch(m_thread, m_workerContext, Kernel::Scheduler::ThreadRunDisposition::Exited,
                                            Ia64BspUserProbeLaunchFailureExitCode, m_workerReceipt, m_workerFailure));
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::WorkerReturn);
        }

        if (launched.GetValue() != Ia64UserModeLaunchDisposition::SupervisorResumed)
        {
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeState);
        }
        const System::Ia64SystemCallWaitSupervisorResult supervisor =
            m_systemCallWaitContinuation.AcceptInitialSupervisorResume();
        if (!supervisor.HasValue())
        {
            m_userProbeFailure.WaitContinuation = supervisor.GetError().Error;
            m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSuspendRejected;
            FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeContinuation);
        }
        System::Ia64SystemCallWaitSupervisorReceipt resumed = supervisor.GetValue();
        Foundation::UInt64 expectedEpoch{1U};
        System::Ia64SystemCallWaitSupervisorResumeSite expectedSite{
            System::Ia64SystemCallWaitSupervisorResumeSite::InitialLaunch};
        for (;;)
        {
            Platform::Synchronization::ProcessorWaitProgress();
            const auto active = RunLoop().GetActiveExecutionBinding();
            if (!active.HasValue())
            {
                m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSuspendRejected;
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeWait);
            }
            const Kernel::Scheduler::ProcessorActiveExecutionBinding& binding = active.GetValue();
            if ((resumed.Site != expectedSite) || (resumed.Epoch != expectedEpoch) ||
                !resumed.Correlation.Thread.IsEqualTo(binding.ThreadIdentity) ||
                !resumed.Correlation.SchedulingContext.IsEqualTo(binding.SchedulingContextIdentity) ||
                (resumed.Correlation.RunSequence != binding.RunSequence) ||
                (resumed.Correlation.Processor.GetValue() != binding.Processor.GetValue()))
            {
                m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSuspendRejected;
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeBinding);
            }
            if (!ArmUserProbeWaitTimer())
            {
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeTimer);
            }

            m_userProbeFailure.WaitContinuation = m_systemCallWaitContinuation.BeginWaitingForReadmission();
            if (m_userProbeFailure.WaitContinuation != System::Ia64SystemCallWaitContinuationError::None)
            {
                m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSuspendRejected;
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeReadmission);
            }

            m_workerReceipt = {};
            m_workerFailure = {};
            if (!RunLoop().FinishWaitAndDispatch(m_thread, m_workerContext, *m_waitSetThreadDispatcher, m_workerReceipt,
                                                 m_workerFailure) ||
                (m_workerReceipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active) ||
                (m_workerReceipt.ThreadStorage != &m_thread))
            {
                m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitSuspendRejected;
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeDispatch);
            }

            resumed = ResumeUserProbeSystemCall();
            Foundation::UInt64 nextEpoch{0U};
            if (!System::Ia64SystemCallWaitEpochPolicy::TryAdvance(expectedEpoch, nextEpoch))
            {
                m_userProbeFailure.Error = Ia64BspKernelRuntimeUserProbeError::WaitResumeRejected;
                FailStopKernelRuntime(Diagnostics::Ia64BootStage::UserProbeEpoch);
            }
            expectedEpoch = nextEpoch;
            expectedSite = System::Ia64SystemCallWaitSupervisorResumeSite::RepeatedSwitch;
        }
    }
    if (m_workerPurpose != WorkerPurpose::BootstrapRoundTrip)
    {
        FailStopKernelRuntime(Diagnostics::Ia64BootStage::WorkerPurpose);
    }

    AsteriaIa64CaptureBspKernelWorkerEvidence(&m_workerEvidence);
    static_cast<void>(RunLoop().FinishAndDispatch(m_thread, m_workerContext,
                                                  Kernel::Scheduler::ThreadRunDisposition::Exited,
                                                  Ia64BspKernelWorkerExitCode, m_workerReceipt, m_workerFailure));
    FailStopKernelRuntime(Diagnostics::Ia64BootStage::WorkerReturn);
}
} // Namespace Asteria::Platform::Ia64::Scheduler

#if ASTERIA_IA64_BSP_NATIVE_PREEMPTION_PROBE_CODE
extern "C" Asteria::Foundation::UInt64 AsteriaIa64BspNativePreemptionCoordinator(void* const argument) noexcept
{
    if (argument == nullptr)
    {
        return 1U;
    }
    auto* const runtime = static_cast<Asteria::Platform::Ia64::Scheduler::Ia64BspKernelRuntime*>(argument);
    return runtime->CoordinateNativeExternalPreemption();
}

extern "C" [[noreturn]] void AsteriaIa64BspNativePreemptionIdleLanding(void* const argument) noexcept
{
    if (argument == nullptr)
    {
        Asteria::Platform::Ia64::Diagnostics::StopIa64(
            Asteria::Kernel::Diagnostics::StopCode::BootStage,
            static_cast<Asteria::Foundation::UInt64>(
                Asteria::Platform::Ia64::Diagnostics::Ia64BootStage::WorkerArgument));
    }
    auto* const runtime = static_cast<Asteria::Platform::Ia64::Scheduler::Ia64BspKernelRuntime*>(argument);
    runtime->LandNativeExternalPreemptionIdle();
}
#endif
