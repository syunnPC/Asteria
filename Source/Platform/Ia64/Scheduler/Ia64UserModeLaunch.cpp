// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64LaunchUserMode(const Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan* plan) noexcept;

// The target entry points are in Ia64UserModeLaunch.S.

namespace Asteria::Platform::Ia64::Scheduler
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 AdapterUnsupportedProcessorState{1U};
constexpr Foundation::UInt64 AdapterInvalidActiveSupervisorState{2U};
constexpr Foundation::UInt64 AdapterNativeTransitionUnavailable{3U};

using ValidationResult = Foundation::Result<Ia64UserModeLaunchPlan, Ia64UserModeLaunchError>;

[[nodiscard]] constexpr ValidationResult Fail(const Ia64UserModeLaunchError error) noexcept
{
    return ValidationResult::Failure(error);
}

[[nodiscard]] constexpr bool TryGetExclusiveEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool IsPageAligned(const Foundation::UInt64 value) noexcept
{
    return (value & (Ia64UserModePageSizeBytes - 1U)) == 0U;
}

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumIa64UserModeImplementedAddressBit)
    {
        return true;
    }

    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumIa64UserModeImplementedAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool IsImplementedRange(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                                const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    return (start < end) && ((start >> 61U) == ((end - 1U) >> 61U)) &&
           IsVirtualAddressImplemented(start, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(end - 1U, implementedMostSignificantBit);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool IsRangeValid(const Ia64UserModeVirtualRange& range,
                                          const Foundation::UInt8 implementedMostSignificantBit,
                                          const bool requireUserRegion, Foundation::UInt64& end) noexcept
{
    return IsPageAligned(range.VirtualStart) && IsPageAligned(range.SizeBytes) &&
           TryGetExclusiveEnd(range.VirtualStart, range.SizeBytes, end) &&
           IsImplementedRange(range.VirtualStart, end, implementedMostSignificantBit) &&
           (requireUserRegion ? ((range.VirtualStart >> 61U) != 0U) : (end <= RegionZeroLimit));
}

[[nodiscard]] constexpr bool IsStorageSizeValid(const Foundation::UInt64 sizeBytes) noexcept
{
    return (sizeBytes >= MinimumIa64UserModeStackSizeBytes) && (sizeBytes <= MaximumIa64UserModeStackSizeBytes);
}

[[nodiscard]] bool IsZeroRegisterState(const Ia64KernelRegisterState& registers) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&registers);
    for (Foundation::Size index{0U}; index < sizeof(registers); ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool RequirementsAreCanonical(const Ia64UserModeLaunchRequirements& requirements) noexcept
{
    if ((requirements.CurrentProcessorStatusMask != Ia64UserModeCurrentProcessorStatusMask) ||
        (requirements.CurrentProcessorStatusValue != Ia64UserModeCurrentProcessorStatusValue) ||
        ((requirements.UserProcessorStatus != Ia64UserModeInitialProcessorStatus) &&
         (requirements.UserProcessorStatus != Ia64UserModePreemptibleProcessorStatus)) ||
        (requirements.CurrentPrivilegeLevel != 0U) || (requirements.CurrentVirtualMachineMode != 0U) ||
        (requirements.CurrentInstructionSet != 0U) || (requirements.CurrentRegisterBank != 1U))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < sizeof(requirements.Reserved); ++index)
    {
        if (requirements.Reserved[index] != 0U)
        {
            return false;
        }
    }
    return true;
}
} // namespace

Foundation::Result<Ia64UserModeLaunchPlan, Ia64UserModeLaunchError>
Ia64UserModeLaunch::Validate(const Ia64UserModeLaunchRequest& request) noexcept
{
    const Foundation::UInt8 implementedMostSignificantBit =
        request.Requirements.ImplementedVirtualAddressMostSignificantBit;
    if ((implementedMostSignificantBit < MinimumIa64UserModeImplementedAddressBit) ||
        (implementedMostSignificantBit > MaximumIa64UserModeImplementedAddressBit))
    {
        return Fail(Ia64UserModeLaunchError::InvalidAddressProfile);
    }
    if (!RequirementsAreCanonical(request.Requirements) || (request.Reserved != 0U))
    {
        return Fail(Ia64UserModeLaunchError::InvalidProcessorStatusRequirements);
    }

    Foundation::UInt64 codeEnd{0U};
    if (!IsRangeValid(request.Code, implementedMostSignificantBit, true, codeEnd))
    {
        return Fail(Ia64UserModeLaunchError::InvalidCodeRange);
    }

    Foundation::UInt64 entryEnd{0U};
    if ((request.EntryBundleAddress & (Ia64UserModeEntryBundleSizeBytes - 1U)) != 0U ||
        !TryGetExclusiveEnd(request.EntryBundleAddress, Ia64UserModeEntryBundleSizeBytes, entryEnd) ||
        (request.EntryBundleAddress < request.Code.VirtualStart) || (entryEnd > codeEnd))
    {
        return Fail(Ia64UserModeLaunchError::InvalidEntryBundle);
    }

    Foundation::UInt64 globalPointerEnd{0U};
    if ((request.InitialGlobalPointer != 0U) &&
        (((request.InitialGlobalPointer & (Ia64UserModeGlobalPointerAlignmentBytes - 1U)) != 0U) ||
         !TryGetExclusiveEnd(request.InitialGlobalPointer, Ia64UserModeGlobalPointerAlignmentBytes, globalPointerEnd) ||
         !IsImplementedRange(request.InitialGlobalPointer, globalPointerEnd, implementedMostSignificantBit) ||
         ((request.InitialGlobalPointer >> 61U) == 0U)))
    {
        return Fail(Ia64UserModeLaunchError::InvalidGlobalPointer);
    }

    Foundation::UInt64 memoryStackEnd{0U};
    if (!IsStorageSizeValid(request.MemoryStack.SizeBytes) ||
        !IsRangeValid(request.MemoryStack, implementedMostSignificantBit, true, memoryStackEnd))
    {
        return Fail(Ia64UserModeLaunchError::InvalidMemoryStack);
    }

    Foundation::UInt64 rseBackingStoreEnd{0U};
    if (!IsStorageSizeValid(request.RseBackingStore.SizeBytes) ||
        !IsRangeValid(request.RseBackingStore, implementedMostSignificantBit, true, rseBackingStoreEnd))
    {
        return Fail(Ia64UserModeLaunchError::InvalidRseBackingStore);
    }

    if (RangesOverlap(request.Code.VirtualStart, codeEnd, request.MemoryStack.VirtualStart, memoryStackEnd) ||
        RangesOverlap(request.Code.VirtualStart, codeEnd, request.RseBackingStore.VirtualStart, rseBackingStoreEnd) ||
        RangesOverlap(request.MemoryStack.VirtualStart, memoryStackEnd, request.RseBackingStore.VirtualStart,
                      rseBackingStoreEnd))
    {
        return Fail(Ia64UserModeLaunchError::UserRangesOverlap);
    }

    Ia64UserModeSupervisorContinuation* const continuation = request.SupervisorContinuation;
    if (continuation == nullptr)
    {
        return Fail(Ia64UserModeLaunchError::InvalidSupervisorContinuation);
    }
    const Foundation::UInt64 continuationStart =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(continuation));
    Foundation::UInt64 continuationEnd{0U};
    if (((continuationStart & (alignof(Ia64UserModeSupervisorContinuation) - 1U)) != 0U) ||
        !TryGetExclusiveEnd(continuationStart, sizeof(*continuation), continuationEnd) ||
        !IsImplementedRange(continuationStart, continuationEnd, implementedMostSignificantBit) ||
        (continuationEnd > RegionZeroLimit) ||
        (continuation->State != Ia64UserModeSupervisorContinuationState::Fresh) ||
        (continuation->Owner != Ia64UserModeSupervisorContinuationOwner::None) ||
        !IsZeroRegisterState(continuation->Registers))
    {
        return Fail(Ia64UserModeLaunchError::InvalidSupervisorContinuation);
    }

    Foundation::UInt64 supervisorMemoryStackEnd{0U};
    if (!IsStorageSizeValid(continuation->MemoryStack.SizeBytes) ||
        !IsRangeValid(continuation->MemoryStack, implementedMostSignificantBit, false, supervisorMemoryStackEnd))
    {
        return Fail(Ia64UserModeLaunchError::InvalidSupervisorMemoryStack);
    }

    Foundation::UInt64 supervisorRseBackingStoreEnd{0U};
    if (!IsStorageSizeValid(continuation->RseBackingStore.SizeBytes) ||
        !IsRangeValid(continuation->RseBackingStore, implementedMostSignificantBit, false,
                      supervisorRseBackingStoreEnd))
    {
        return Fail(Ia64UserModeLaunchError::InvalidSupervisorRseBackingStore);
    }

    if (RangesOverlap(continuation->MemoryStack.VirtualStart, supervisorMemoryStackEnd,
                      continuation->RseBackingStore.VirtualStart, supervisorRseBackingStoreEnd))
    {
        return Fail(Ia64UserModeLaunchError::SupervisorRangesOverlap);
    }
    if (RangesOverlap(continuationStart, continuationEnd, continuation->MemoryStack.VirtualStart,
                      supervisorMemoryStackEnd) ||
        RangesOverlap(continuationStart, continuationEnd, continuation->RseBackingStore.VirtualStart,
                      supervisorRseBackingStoreEnd))
    {
        return Fail(Ia64UserModeLaunchError::SupervisorContinuationAliasesStorage);
    }

    const Foundation::UInt64 userStackPointer = memoryStackEnd - Ia64UserModeInitialStackScratchBytes;
    return ValidationResult::Success(Ia64UserModeLaunchPlan{
        .EntryBundleAddress = request.EntryBundleAddress,
        .UserStackPointer = userStackPointer,
        .UserRseBackingStorePointer = request.RseBackingStore.VirtualStart,
        .UserProcessorStatus = request.Requirements.UserProcessorStatus,
        .UserRseConfiguration = Ia64UserModeInitialRseConfiguration,
        .CurrentProcessorStatusMask = Ia64UserModeCurrentProcessorStatusMask,
        .CurrentProcessorStatusValue = Ia64UserModeCurrentProcessorStatusValue,
        .SupervisorContinuationAddress = continuationStart,
        .InitialArguments = request.InitialArguments,
        .InitialGlobalPointer = request.InitialGlobalPointer,
    });
}

Ia64UserModeLaunchError Ia64UserModeLaunch::Launch(const Ia64UserModeLaunchRequest& request) noexcept
{
    const Ia64UserModeLaunchResult result = LaunchResumable(request);
    return result.HasValue() ? Ia64UserModeLaunchError::UnexpectedAdapterReturn : result.GetError();
}

Ia64UserModeLaunchResult Ia64UserModeLaunch::LaunchResumable(const Ia64UserModeLaunchRequest& request) noexcept
{
    const ValidationResult validation = Validate(request);
    if (!validation.HasValue())
    {
        return Ia64UserModeLaunchResult::Failure(validation.GetError());
    }

    const Ia64UserModeLaunchPlan plan = validation.GetValue();
    const Foundation::UInt64 adapterStatus = AsteriaIa64LaunchUserMode(&plan);
    if (request.SupervisorContinuation->State == Ia64UserModeSupervisorContinuationState::ResumePending)
    {
        return Ia64UserModeLaunchResult::Success(Ia64UserModeLaunchDisposition::SupervisorResumed);
    }
    if (adapterStatus == AdapterUnsupportedProcessorState)
    {
        return Ia64UserModeLaunchResult::Failure(Ia64UserModeLaunchError::UnsupportedProcessorState);
    }
    if (adapterStatus == AdapterInvalidActiveSupervisorState)
    {
        return Ia64UserModeLaunchResult::Failure(Ia64UserModeLaunchError::InvalidActiveSupervisorState);
    }
    if (adapterStatus == AdapterNativeTransitionUnavailable)
    {
        return Ia64UserModeLaunchResult::Failure(Ia64UserModeLaunchError::NativeTransitionUnavailable);
    }
    return Ia64UserModeLaunchResult::Failure(Ia64UserModeLaunchError::UnexpectedAdapterReturn);
}
} // namespace Asteria::Platform::Ia64::Scheduler
