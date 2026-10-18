// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_STOP_STAGE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_STOP_STAGE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Diagnostics
{
enum class Amd64BootStage : Foundation::UInt64
{
    ApOwner = 1U,
    ApContext = 2U,
    UserStack = 3U,
    Root = 4U,
    Apic = 5U,
    Endpoint = 6U,
    WaitObjects = 7U,
    Devices = 8U,
    Iommu = 9U,
    Processors = 10U,
    Shootdown = 11U,
    Management = 12U,
    Pager = 13U,
    Spinner = 14U,
    Thread = 15U,
    Admission = 16U,
    RegistryBudget = 17U,
    Reap = 18U,
    DispatchLate = 19U,
    WaitIdle = 20U,
    WaitRearm = 21U,
    DispatchReturn = 22U,
    PagerIdle = 23U,
    ThreadWait = 24U,
    IdleStall = 25U,
    TimerCpu = 26U,
    UserTick = 27U,
    UserFault = 28U,
    UserException = 29U,
    Syscall = 30U,
    ResumeFrame = 31U,
    ResumeStack = 32U,
    NotPrepared = 33U,
    NoTrapOwner = 34U,
    PagerStack = 35U,
    SpinnerStack = 36U,
    CreatedThreadStack = 37U,
    MmioWindowInvariant = 38U,
    NativePreemptionInput = 100U,
    NativePreemptionPrepareAdmission = 101U,
    NativePreemptionDispatch = 102U,
    NativePreemptionArm = 103U,
    NativePreemptionResumeState = 104U,
    NativePreemptionWorkerExit = 105U,
    NativePreemptionCaptureState = 106U,
    NativePreemptionCaptureDeadline = 107U,
    NativePreemptionStopRun = 108U,
    NativePreemptionReadmission = 109U,
    NativePreemptionIdleLanding = 110U,
    NativePreemptionCompletionReturn = 111U,
    NativePreemptionCoordinatorOwner = 112U,
    DeviceScanout = 200U,
    DeviceRead = 201U,
    DeviceReadReceipt = 202U,
    DeviceIdentify = 203U,
    DeviceIdentifyReceipt = 204U,
    DeviceCapabilityKey = 205U,
    DeviceEcam = 206U,
    DeviceAssignable = 207U,
    DeviceNicAssignable = 208U,
    DeviceAhciAssignable = 209U,
    DeviceAhciBinding = 210U,
};
struct Amd64StageName
{
    Foundation::UInt64 Id;
    const char* Name;
};
inline constexpr Amd64StageName Amd64BootStageNames[]{
    {1U, "ApOwner"},
    {2U, "ApContext"},
    {3U, "UserStack"},
    {4U, "Root"},
    {5U, "Apic"},
    {6U, "Endpoint"},
    {7U, "WaitObjects"},
    {8U, "Devices"},
    {9U, "Iommu"},
    {10U, "Processors"},
    {11U, "Shootdown"},
    {12U, "Management"},
    {13U, "Pager"},
    {14U, "Spinner"},
    {15U, "Thread"},
    {16U, "Admission"},
    {17U, "RegistryBudget"},
    {18U, "Reap"},
    {19U, "DispatchLate"},
    {20U, "WaitIdle"},
    {21U, "WaitRearm"},
    {22U, "DispatchReturn"},
    {23U, "PagerIdle"},
    {24U, "ThreadWait"},
    {25U, "IdleStall"},
    {26U, "TimerCpu"},
    {27U, "UserTick"},
    {28U, "UserFault"},
    {29U, "UserException"},
    {30U, "Syscall"},
    {31U, "ResumeFrame"},
    {32U, "ResumeStack"},
    {33U, "NotPrepared"},
    {34U, "NoTrapOwner"},
    {35U, "PagerStack"},
    {36U, "SpinnerStack"},
    {37U, "CreatedThreadStack"},
    {38U, "MmioWindowInvariant"},
    {100U, "NativePreemptionInput"},
    {101U, "NativePreemptionPrepareAdmission"},
    {102U, "NativePreemptionDispatch"},
    {103U, "NativePreemptionArm"},
    {104U, "NativePreemptionResumeState"},
    {105U, "NativePreemptionWorkerExit"},
    {106U, "NativePreemptionCaptureState"},
    {107U, "NativePreemptionCaptureDeadline"},
    {108U, "NativePreemptionStopRun"},
    {109U, "NativePreemptionReadmission"},
    {110U, "NativePreemptionIdleLanding"},
    {111U, "NativePreemptionCompletionReturn"},
    {112U, "NativePreemptionCoordinatorOwner"},
    {200U, "DeviceScanout"},
    {201U, "DeviceRead"},
    {202U, "DeviceReadReceipt"},
    {203U, "DeviceIdentify"},
    {204U, "DeviceIdentifyReceipt"},
    {205U, "DeviceCapabilityKey"},
    {206U, "DeviceEcam"},
    {207U, "DeviceAssignable"},
    {208U, "DeviceNicAssignable"},
    {209U, "DeviceAhciAssignable"},
    {210U, "DeviceAhciBinding"},
};
inline constexpr Amd64StageName Amd64ApplicationProcessorStageNames[]{
    {1U, "EntryAndTimer"}, {2U, "ObjectsAndAdmission"}, {3U, "InitialDispatch"}, {4U, "TimerDelivery"},
    {5U, "RunCounter"},    {6U, "WorkerYield"},         {7U, "ShootdownDrain"},  {8U, "ServiceDispatch"},
};
[[nodiscard]] constexpr const char* Amd64StageNameFor(const bool applicationProcessor,
                                                      const Foundation::UInt64 id) noexcept
{
    if (applicationProcessor)
    {
        for (const auto& entry : Amd64ApplicationProcessorStageNames)
        {
            if (entry.Id == id)
            {
                return entry.Name;
            }
        }
    }
    else
    {
        for (const auto& entry : Amd64BootStageNames)
        {
            if (entry.Id == id)
            {
                return entry.Name;
            }
        }
    }
    return "UnknownStage";
}
} // namespace Asteria::Platform::Amd64::Diagnostics

#endif // ASTERIA_PLATFORM_AMD64_DIAGNOSTICS_AMD64_STOP_STAGE_HPP_INCLUDED
