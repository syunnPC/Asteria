// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_STOP_STAGE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_STOP_STAGE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Diagnostics
{
enum class Ia64BootStage : Foundation::UInt64
{
    WaitCompletion = 1U,
    WaitResume = 2U,
    TimerPreparationRollback = 3U,
    TimerPollRollback = 4U,
    TimerPoll = 5U,
    TimerDelivery = 6U,
    TimerRefresh = 7U,
    TimerNotification = 8U,
    NativeWorkerReturn = 9U,
    NativeIdleState = 10U,
    NativeIdleDispatch = 11U,
    NativeIdleControl = 12U,
    DeviceInterrupt = 13U,
    Reap = 14U,
    DispatchLate = 15U,
    Replenishment = 16U,
    IdleStall = 17U,
    WorkerArgument = 18U,
    InitialRoot = 19U,
    InitialLaunch = 20U,
    InitialWorkerReturn = 21U,
    UserProbeRoot = 22U,
    UserProbeLaunch = 23U,
    UserProbeWait = 24U,
    UserProbeState = 25U,
    UserProbeContinuation = 26U,
    UserProbeBinding = 27U,
    UserProbeTimer = 28U,
    UserProbeReadmission = 29U,
    UserProbeDispatch = 30U,
    UserProbeEpoch = 31U,
    WorkerPurpose = 32U,
    WorkerReturn = 33U,
    KernelPreparation = 100U,
    DevicePublication = 101U,
    DeviceRead = 102U,
    DeviceBinding = 103U,
    Endpoint = 104U,
    Management = 105U,
    ThreadTable = 106U,
    Pager = 107U,
    ApplicationProcessors = 108U,
    InitialObjects = 109U,
    InitialMappings = 110U,
    UserPreparation = 111U,
    UserDispatch = 112U,
    SyntheticProbe = 113U,
    NativePreemption = 114U,
    ProcessorAdmission = 115U,
};
enum class Ia64ApplicationProcessorStage : Foundation::UInt64
{
    IdleProcessor = 1U,
    ShootdownDrain = 2U,
    TimerServiceConflict = 3U,
    TimerServiceBegin = 4U,
    TimerServiceComplete = 5U,
    SchedulerService = 6U,
    LandingProcessor = 7U,
    LandingEntry = 8U,
    TimerProofBegin = 9U,
    TimerProofComplete = 10U,
    SchedulerPrepare = 11U,
    SchedulerDispatch = 12U,
    WorkerArgument = 13U,
    WorkerOwner = 14U,
    WorkerReserved = 15U,
    WorkerGeneration = 16U,
    WorkerPublication = 17U,
    WorkerFinish = 18U,
    ServiceCompletion = 19U,
};
struct Ia64StageName
{
    Foundation::UInt64 Id;
    const char* Name;
};
inline constexpr Ia64StageName Ia64BootStageNames[]{
    {1U, "WaitCompletion"},
    {2U, "WaitResume"},
    {3U, "TimerPreparationRollback"},
    {4U, "TimerPollRollback"},
    {5U, "TimerPoll"},
    {6U, "TimerDelivery"},
    {7U, "TimerRefresh"},
    {8U, "TimerNotification"},
    {9U, "NativeWorkerReturn"},
    {10U, "NativeIdleState"},
    {11U, "NativeIdleDispatch"},
    {12U, "NativeIdleControl"},
    {13U, "DeviceInterrupt"},
    {14U, "Reap"},
    {15U, "DispatchLate"},
    {16U, "Replenishment"},
    {17U, "IdleStall"},
    {18U, "WorkerArgument"},
    {19U, "InitialRoot"},
    {20U, "InitialLaunch"},
    {21U, "InitialWorkerReturn"},
    {22U, "UserProbeRoot"},
    {23U, "UserProbeLaunch"},
    {24U, "UserProbeWait"},
    {25U, "UserProbeState"},
    {26U, "UserProbeContinuation"},
    {27U, "UserProbeBinding"},
    {28U, "UserProbeTimer"},
    {29U, "UserProbeReadmission"},
    {30U, "UserProbeDispatch"},
    {31U, "UserProbeEpoch"},
    {32U, "WorkerPurpose"},
    {33U, "WorkerReturn"},
    {100U, "KernelPreparation"},
    {101U, "DevicePublication"},
    {102U, "DeviceRead"},
    {103U, "DeviceBinding"},
    {104U, "Endpoint"},
    {105U, "Management"},
    {106U, "ThreadTable"},
    {107U, "Pager"},
    {108U, "ApplicationProcessors"},
    {109U, "InitialObjects"},
    {110U, "InitialMappings"},
    {111U, "UserPreparation"},
    {112U, "UserDispatch"},
    {113U, "SyntheticProbe"},
    {114U, "NativePreemption"},
    {115U, "ProcessorAdmission"},
};
inline constexpr Ia64StageName Ia64ApplicationProcessorStageNames[]{
    {1U, "IdleProcessor"},        {2U, "ShootdownDrain"},      {3U, "TimerServiceConflict"}, {4U, "TimerServiceBegin"},
    {5U, "TimerServiceComplete"}, {6U, "SchedulerService"},    {7U, "LandingProcessor"},     {8U, "LandingEntry"},
    {9U, "TimerProofBegin"},      {10U, "TimerProofComplete"}, {11U, "SchedulerPrepare"},    {12U, "SchedulerDispatch"},
    {13U, "WorkerArgument"},      {14U, "WorkerOwner"},        {15U, "WorkerReserved"},      {16U, "WorkerGeneration"},
    {17U, "WorkerPublication"},   {18U, "WorkerFinish"},       {19U, "ServiceCompletion"},
};
[[nodiscard]] constexpr const char* Ia64StageNameFor(const bool applicationProcessor,
                                                     const Foundation::UInt64 id) noexcept
{
    if (applicationProcessor)
    {
        for (const auto& stage : Ia64ApplicationProcessorStageNames)
        {
            if (stage.Id == id)
            {
                return stage.Name;
            }
        }
    }
    else
    {
        for (const auto& stage : Ia64BootStageNames)
        {
            if (stage.Id == id)
            {
                return stage.Name;
            }
        }
    }
    return "UnknownStage";
}
} // namespace Asteria::Platform::Ia64::Diagnostics

#endif // ASTERIA_PLATFORM_IA64_DIAGNOSTICS_IA64_STOP_STAGE_HPP_INCLUDED
