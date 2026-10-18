// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_DRIVER_SETUP_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_DRIVER_SETUP_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/ObjectRequests.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// What a driver component sets up in its own domain before it drives its device: its registers in
/// its window, its DMA pages, and the IommuDomain that confines its function; the drain that tears
/// the driver down takes all of it. The edu drivers (ComponentProgram.cpp), the AHCI driver
/// (AhciDriver.cpp) and the NIC driver (NicDriver.cpp) share these steps. A driver's exit code for
/// the step (Abi::ComponentDriver*Step) that was not as its device requires carries the error the
/// kernel answered.
[[nodiscard]] constexpr Foundation::UInt64 DriverFailure(const Foundation::UInt64 step,
                                                         const Abi::SystemCallError error) noexcept
{
    return Abi::ComponentDriverFailedExitCode | (step << 32U) | (static_cast<Foundation::UInt64>(error) << 40U);
}

/// What the DMA steps answer, in place of zero or a failure, on a kernel with no IOMMU, whose
/// IommuDomain creation is not configured: the device may never master the bus, so the driver does
/// no DMA. No failure's exit code is this small.
inline constexpr Foundation::UInt64 DriverWithoutIommu{1U};

/// The first page of the driver's window, where MapRegisters maps its
/// device's registers.
[[nodiscard]] inline volatile Foundation::UInt32*
RegistersOf(const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    return reinterpret_cast<volatile Foundation::UInt32*>(
        static_cast<Foundation::UIntPointer>(startBlock[Abi::ComponentWindowStartWord]));
}

/// Maps the first pages pages of the device's registers at the start of the window through a device
/// Frame and an uncached MemoryObject of the driver's own domain; the drain takes the mapping.
/// Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] inline Foundation::UInt64
MapRegisters(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
             const Foundation::UInt64 pages = 1U) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{startBlock[Abi::ComponentAccountSpaceWord],
                                         startBlock[Abi::ComponentAccountSpaceWord + 1U]};
    const System::CapabilityHandle region{startBlock[Abi::ComponentDeviceRegionWord],
                                          startBlock[Abi::ComponentDeviceRegionWord + 1U]};
    const Abi::SystemCallResponse frame = System::CreateDeviceFrame(client, domain, region, pages);
    if (frame.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverFrameStep, frame.Error);
    }
    const Abi::SystemCallResponse memory =
        System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, pages, Abi::ObjectCreateCacheUncached);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMemoryObjectStep, memory.Error);
    }
    const Abi::SystemCallError mapped = System::MapWindow(
        client, space, System::CapabilityHandle{memory.Value0, memory.Value1},
        System::CapabilityHandle{frame.Value0, frame.Value1}, startBlock[Abi::ComponentWindowStartWord], pages);
    return (mapped == Abi::SystemCallError::None) ? 0U : DriverFailure(Abi::ComponentDriverMapStep, mapped);
}

/// A Frame of pages pages (one by default) of the driver's own domain mapped at address, its Frame
/// capability in frame. A nonzero alignmentPages asks for a Frame at a multiple of that many pages,
/// the device address of its pages on the trusted-driver profile.
[[nodiscard, gnu::noinline]] inline Abi::SystemCallError
MapDmaPage(const System::SystemCallClient& client, const System::CapabilityHandle domain,
           const System::CapabilityHandle space, const Foundation::UInt64 address, System::CapabilityHandle& frame,
           const Foundation::UInt64 pages = 1U, const Foundation::UInt8 alignmentPages = 0U) noexcept
{
    const Abi::SystemCallResponse created = System::CreateMemory(client, domain, Abi::ObjectTypeFrame, pages,
                                                                 Abi::ObjectCreateCacheWriteBack, alignmentPages);
    const Abi::SystemCallResponse memory =
        (created.Error == Abi::SystemCallError::None)
            ? System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, pages)
            : created;
    if (memory.Error != Abi::SystemCallError::None)
    {
        return memory.Error;
    }
    frame = System::CapabilityHandle{created.Value0, created.Value1};
    return System::MapWindow(client, space, System::CapabilityHandle{memory.Value0, memory.Value1}, frame, address,
                             pages);
}

/// An IommuMap's outcome: its error, and the device address the mapping
/// starts at, which the driver programs and IommuUnmap names.
struct DeviceMapping
{
    Abi::SystemCallError Error;
    Foundation::UInt64 Address;
};

/// Maps the first pages pages of frame (Map) of the IommuDomain iommu names, with permissions (Abi:
/// IommuMapDevice*), asking for requestedAddress. The kernel answers the device address the device
/// must be given: the requested one behind a translating IOMMU, the Frame's physical address on the
/// trusted-driver profile's identity backend, so a driver never programs a constant.
[[nodiscard, gnu::noinline]] inline DeviceMapping
MapForDevice(const System::SystemCallClient& client, const System::CapabilityHandle iommu,
             const System::CapabilityHandle frame, const Foundation::UInt64 pages,
             const Foundation::UInt64 requestedAddress, const Foundation::UInt8 permissions) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::IommuMapRequestSizeBytes]{};
    System::StoreHandle(request + Abi::IommuMapFrameOffset, frame);
    System::StoreWord(request + Abi::IommuMapPagesOffset, pages);
    System::StoreWord(request + Abi::IommuMapDeviceAddressOffset, requestedAddress);
    request[Abi::IommuMapPermissionsOffset] = permissions;
    const Abi::SystemCallResponse response =
        client.ControlIommu(iommu, Abi::IommuMap, System::AddressOf(request), sizeof(request));
    return DeviceMapping{.Error = response.Error,
                         .Address = (response.Error == Abi::SystemCallError::None) ? response.Value0 : 0U};
}

/// Creates an IommuDomain of the device's DMA address bits in the driver's own domain, its
/// capability in iommu, and attaches the driver's function. Zero, DriverWithoutIommu on a kernel
/// without an IOMMU (for a function BOOT.CFG trusts there, the kernel creates one of the
/// trusted-driver profile instead), or the failure's exit code.
[[nodiscard, gnu::noinline]] inline Foundation::UInt64
ConfineFunction(const System::SystemCallClient& client,
                const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords], System::CapabilityHandle& iommu,
                const Foundation::UInt8 addressBits = 32U) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateIommuDomainRequestSizeBytes]{};
    request[Abi::ObjectCreateIommuAddressBitsOffset] = addressBits;
    const Abi::SystemCallResponse created =
        client.CreateObject(domain, Abi::ObjectTypeIommuDomain, System::AddressOf(request), sizeof(request));
    if (created.Error == Abi::SystemCallError::InvalidConfiguration)
    {
        return DriverWithoutIommu;
    }
    if (created.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverIommuStep, created.Error);
    }
    iommu = System::CapabilityHandle{created.Value0, created.Value1};
    const Abi::SystemCallError attached =
        client.ControlIommu(iommu, Abi::IommuAttach, function.Selector, function.Authenticator).Error;
    return (attached == Abi::SystemCallError::None) ? 0U : DriverFailure(Abi::ComponentDriverAttachStep, attached);
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_DRIVER_SETUP_HPP_INCLUDED
