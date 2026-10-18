// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/SalBootRendezvousRegistration.hpp"

extern "C" Asteria::Platform::Ia64::Firmware::SalFirmwareCallResult
AsteriaIa64InvokePhysicalSalSetVectors(const Asteria::Platform::Ia64::Firmware::SalFirmwareCallRequest* request,
                                       Asteria::Foundation::UInt64* invocationState) noexcept;

// The target entry point is in SalBootRendezvousRegistration.S.

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumPageCount{Foundation::UInt64{1U} << 52U};
constexpr Foundation::UInt64 PhysicalAddressTopBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 MaximumPhysicalAddress{PhysicalAddressTopBit - 1U};
constexpr Foundation::UInt64 Ia64RuntimeAlignmentBytes{8192U};
constexpr Foundation::UInt64 Ia64RuntimeAlignmentMask{Ia64RuntimeAlignmentBytes - 1U};
constexpr Foundation::UInt64 RuntimeMemoryAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 EntrypointAccessBytes{16U};
constexpr Foundation::UInt64 GlobalDataAccessBytes{8U};
constexpr Foundation::UInt32 RuntimeServicesCodeMemoryType{5U};
constexpr Foundation::UInt32 RuntimeServicesDataMemoryType{6U};
constexpr Foundation::UInt32 PalCodeMemoryType{13U};
constexpr Foundation::UInt64 SalSetVectorsFunctionId{0x0100'0000U};
constexpr Foundation::UInt64 OsBootRendezvousVectorType{2U};
constexpr Foundation::Int64 SalNotImplementedStatus{-1};
constexpr Foundation::Int64 SalInvalidArgumentStatus{-2};
constexpr Foundation::Int64 SalCallFailedStatus{-3};
constexpr Foundation::Int64 SalVirtualAddressNotRegisteredStatus{-4};

[[nodiscard]] constexpr SalBootRendezvousRegistrationResult
MakeResult(const SalBootRendezvousRegistrationStatus status, const SalBootRendezvousRegistrationDisposition disposition,
           const Foundation::Int64 firmwareStatus = 0) noexcept
{
    return SalBootRendezvousRegistrationResult{
        .Status = status,
        .Disposition = disposition,
        .Reserved = {0U, 0U, 0U},
        .FirmwareStatus = firmwareStatus,
    };
}

[[nodiscard]] bool IsBcdByte(const Foundation::UInt8 value) noexcept
{
    return ((value & 0x0FU) <= 9U) && ((value >> 4U) <= 9U);
}

[[nodiscard]] bool IsSupportedRevision(const Foundation::UInt16 revision) noexcept
{
    const Foundation::UInt8 minor = static_cast<Foundation::UInt8>(revision & 0xFFU);
    const Foundation::UInt8 major = static_cast<Foundation::UInt8>(revision >> 8U);
    return IsBcdByte(minor) && IsBcdByte(major) && (((major == 0x02U) && (minor >= 0x80U)) || (major == 0x03U));
}

[[nodiscard]] bool IsPhysicalAddress(const Foundation::UInt64 address, const Foundation::UInt64 alignment) noexcept
{
    return (address != 0U) && ((address & PhysicalAddressTopBit) == 0U) && ((address & (alignment - 1U)) == 0U);
}

[[nodiscard]] bool TryGetRegionEnd(const Boot::PhysicalMemoryRegion& region, Foundation::UInt64& physicalEnd) noexcept
{
    if ((region.PageCount == 0U) || (region.PageCount > MaximumPageCount) ||
        ((region.PhysicalStart & PageOffsetMask) != 0U) ||
        ((region.VirtualStart != 0U) && ((region.VirtualStart & PageOffsetMask) != 0U)))
    {
        return false;
    }

    if (region.PageCount == MaximumPageCount)
    {
        if ((region.PhysicalStart != 0U) || (region.VirtualStart != 0U))
        {
            return false;
        }
        physicalEnd = MaximumUInt64;
        return true;
    }

    const Foundation::UInt64 lastByteOffset = (region.PageCount << 12U) - 1U;
    if ((region.PhysicalStart > (MaximumUInt64 - lastByteOffset)) ||
        ((region.VirtualStart != 0U) && (region.VirtualStart > (MaximumUInt64 - lastByteOffset))))
    {
        return false;
    }
    physicalEnd = region.PhysicalStart + lastByteOffset;
    return true;
}

[[nodiscard]] bool IsAccessContained(const Foundation::UInt64 address, const Foundation::UInt64 accessBytes,
                                     const Boot::PhysicalMemoryRegion& region) noexcept
{
    if ((accessBytes == 0U) || (address > (MaximumPhysicalAddress - (accessBytes - 1U))))
    {
        return false;
    }

    Foundation::UInt64 regionEnd{0U};
    if (!TryGetRegionEnd(region, regionEnd))
    {
        return false;
    }
    const Foundation::UInt64 accessEnd = address + accessBytes - 1U;
    return (address >= region.PhysicalStart) && (accessEnd <= regionEnd);
}

[[nodiscard]] bool IsRuntimeRegionValid(const Boot::PhysicalMemoryRegion& region) noexcept
{
    return ((region.Attributes & RuntimeMemoryAttribute) != 0U) &&
           ((region.PhysicalStart & Ia64RuntimeAlignmentMask) == 0U) &&
           ((region.VirtualStart == 0U) || ((region.VirtualStart & Ia64RuntimeAlignmentMask) == 0U)) &&
           ((region.PageCount & Foundation::UInt64{1U}) == 0U);
}

[[nodiscard]] bool IsBindingValid(const SalEntrypointBinding& binding) noexcept
{
    const bool palValid =
        IsPhysicalAddress(binding.PalProcedurePhysicalAddress, 16U) &&
        (binding.PalCodeRegion.FirmwareType == PalCodeMemoryType) &&
        (binding.PalCodeRegion.Kind == Boot::PhysicalMemoryKind::PalCode) &&
        IsAccessContained(binding.PalProcedurePhysicalAddress, EntrypointAccessBytes, binding.PalCodeRegion);
    const bool salCodeValid =
        IsPhysicalAddress(binding.SalProcedurePhysicalAddress, 16U) &&
        (binding.SalCodeRegion.FirmwareType == RuntimeServicesCodeMemoryType) &&
        (binding.SalCodeRegion.Kind == Boot::PhysicalMemoryKind::RuntimeServices) &&
        IsRuntimeRegionValid(binding.SalCodeRegion) &&
        IsAccessContained(binding.SalProcedurePhysicalAddress, EntrypointAccessBytes, binding.SalCodeRegion);
    const bool salDataTypeValid = (binding.SalGlobalDataRegion.FirmwareType == RuntimeServicesCodeMemoryType) ||
                                  (binding.SalGlobalDataRegion.FirmwareType == RuntimeServicesDataMemoryType);
    const bool salDataValid =
        IsPhysicalAddress(binding.SalGlobalDataPhysicalAddress, 1U) && salDataTypeValid &&
        (binding.SalGlobalDataRegion.Kind == Boot::PhysicalMemoryKind::RuntimeServices) &&
        IsRuntimeRegionValid(binding.SalGlobalDataRegion) &&
        IsAccessContained(binding.SalGlobalDataPhysicalAddress, GlobalDataAccessBytes, binding.SalGlobalDataRegion);
    return palValid && salCodeValid && salDataValid;
}

[[nodiscard]] bool IsSalInformationValid(const SalSystemTableInformation& information) noexcept
{
    return (information.Reserved8 == 0U) && IsSupportedRevision(information.SalRevision) &&
           IsPhysicalAddress(information.PalProcedurePhysicalAddress, 16U) &&
           IsPhysicalAddress(information.SalProcedurePhysicalAddress, 16U) &&
           IsPhysicalAddress(information.SalGlobalDataPhysicalAddress, 1U);
}

[[nodiscard]] bool DoesBindingMatchInformation(const SalEntrypointBinding& binding,
                                               const SalSystemTableInformation& information) noexcept
{
    return (binding.PalProcedurePhysicalAddress == information.PalProcedurePhysicalAddress) &&
           (binding.SalProcedurePhysicalAddress == information.SalProcedurePhysicalAddress) &&
           (binding.SalGlobalDataPhysicalAddress == information.SalGlobalDataPhysicalAddress);
}

[[nodiscard]] bool IsHandlerCodeExtentValid(const SalBootRendezvousHandler& handler) noexcept
{
    if (!IsPhysicalAddress(handler.CodePhysicalStart, 16U) || (handler.CodeSizeBytes < EntrypointAccessBytes) ||
        (handler.CodeSizeBytes > PhysicalAddressTopBit) ||
        ((handler.CodeSizeBytes & (EntrypointAccessBytes - 1U)) != 0U) ||
        (handler.CodePhysicalStart > (MaximumPhysicalAddress - (handler.CodeSizeBytes - 1U))))
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool IsHandlerEntrypointValid(const SalBootRendezvousHandler& handler) noexcept
{
    if (!IsPhysicalAddress(handler.EntryPhysicalAddress, 16U) || !IsHandlerCodeExtentValid(handler))
    {
        return false;
    }
    const Foundation::UInt64 codeEnd = handler.CodePhysicalStart + handler.CodeSizeBytes - 1U;
    const Foundation::UInt64 entryEnd = handler.EntryPhysicalAddress + EntrypointAccessBytes - 1U;
    return (handler.EntryPhysicalAddress >= handler.CodePhysicalStart) && (entryEnd <= codeEnd);
}

[[nodiscard]] constexpr SalBootRendezvousRegistrationResult
MakeFirmwareFailureResult(const Foundation::Int64 status) noexcept
{
    switch (status)
    {
    case SalNotImplementedStatus:
        return MakeResult(SalBootRendezvousRegistrationStatus::FirmwareNotImplemented,
                          SalBootRendezvousRegistrationDisposition::NotRegistered, status);
    case SalInvalidArgumentStatus:
        return MakeResult(SalBootRendezvousRegistrationStatus::FirmwareInvalidArgument,
                          SalBootRendezvousRegistrationDisposition::NotRegistered, status);
    case SalCallFailedStatus:
        return MakeResult(SalBootRendezvousRegistrationStatus::FirmwareCallFailed,
                          SalBootRendezvousRegistrationDisposition::Indeterminate, status);
    case SalVirtualAddressNotRegisteredStatus:
        return MakeResult(SalBootRendezvousRegistrationStatus::FirmwareVirtualAddressNotRegistered,
                          SalBootRendezvousRegistrationDisposition::NotRegistered, status);
    default:
        return MakeResult(SalBootRendezvousRegistrationStatus::UnexpectedFirmwareStatus,
                          SalBootRendezvousRegistrationDisposition::Indeterminate, status);
    }
}
} // namespace

SalBootRendezvousRegistrationResult
SalBootRendezvousRegistration::Register(const SalEntrypointBinding& binding,
                                        const SalSystemTableInformation& salInformation,
                                        const SalBootRendezvousHandler& handler) noexcept
{
    const SalEntrypointBinding bindingSnapshot = binding;
    const SalSystemTableInformation informationSnapshot = salInformation;
    const SalBootRendezvousHandler handlerSnapshot = handler;

    if (!IsSalInformationValid(informationSnapshot))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::InvalidSalInformation,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }
    if (!IsBindingValid(bindingSnapshot))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::InvalidBinding,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }
    if (!DoesBindingMatchInformation(bindingSnapshot, informationSnapshot))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::BindingInformationMismatch,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }
    if (!IsHandlerCodeExtentValid(handlerSnapshot))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::InvalidHandlerCodeExtent,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }
    if (!IsHandlerEntrypointValid(handlerSnapshot))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::InvalidHandlerEntrypoint,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }
    if (!IsPhysicalAddress(handlerSnapshot.GlobalPointerPhysicalAddress, GlobalDataAccessBytes))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::InvalidHandlerGlobalPointer,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }

    const SalFirmwareCallRequest request{
        .EntryPhysicalAddress = bindingSnapshot.SalProcedurePhysicalAddress,
        .GlobalDataPhysicalAddress = bindingSnapshot.SalGlobalDataPhysicalAddress,
        .Arguments =
            {
                SalSetVectorsFunctionId,
                OsBootRendezvousVectorType,
                handlerSnapshot.EntryPhysicalAddress,
                handlerSnapshot.GlobalPointerPhysicalAddress,
                0U,
                0U,
                0U,
                0U,
            },
    };
    Foundation::UInt64 invocationState{~Foundation::UInt64{0U}};
    const SalFirmwareCallResult firmwareResult = AsteriaIa64InvokePhysicalSalSetVectors(&request, &invocationState);
    if (invocationState != 0U)
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::UnsupportedProcessorState,
                          SalBootRendezvousRegistrationDisposition::NotRegistered);
    }
    if ((firmwareResult.Result1 != 0U) || (firmwareResult.Result2 != 0U) || (firmwareResult.Result3 != 0U))
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::InvalidFirmwareReturn,
                          SalBootRendezvousRegistrationDisposition::Indeterminate, firmwareResult.Status);
    }
    if (firmwareResult.Status == 0)
    {
        return MakeResult(SalBootRendezvousRegistrationStatus::Registered,
                          SalBootRendezvousRegistrationDisposition::Registered, firmwareResult.Status);
    }
    return MakeFirmwareFailureResult(firmwareResult.Status);
}
} // namespace Asteria::Platform::Ia64::Firmware
