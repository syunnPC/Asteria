// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_IOMMU_DOMAIN_BACKEND_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_IOMMU_DOMAIN_BACKEND_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/DmaTypes.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Kernel::Device
{
class IommuDomain;
}

namespace Asteria::Hal::Device
{
class IIommuDevice;

enum class IommuBackendOwnershipError : Foundation::UInt8
{
    None,
    NotReady,
    AlreadyOwned,
    InvalidOwner,
    UnsupportedConfiguration,
    HardwareFailure,
};

enum class IommuBackendError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidRequest,
    UnsupportedConfiguration,
    CapacityExceeded,
    HardwareFailure,
    CommittedStateUnknown,
    CorruptState,
};

struct IommuBackendMapRequest
{
    Kernel::Device::DmaAddress DmaStart;
    Kernel::Memory::PhysicalAddress PhysicalStart;
    Kernel::Memory::PageCount Pages;
    Kernel::Device::DmaPermissions Permissions;
    Foundation::UInt8 Reserved[7];
};

/// The DMA faults the native layer recorded for one domain's requesters: how
/// many, and the last one's requester identifier and native reason.
struct IommuBackendFaults
{
    Foundation::UInt64 Count;
    Foundation::UInt16 LastSource;
    Foundation::UInt8 LastReason;
    Foundation::UInt8 Reserved[5];
    Foundation::UInt64 LastAddress{0U};
};

/// Owns the architecture-specific translation state for one IommuDomain. ChooseDeviceAddress names
/// the device address a mapping of a physical range takes, which the driver programs and
/// IommuDomain returns from Map: a translating backend (VT-d, AMD-Vi, virtio-iommu) takes the
/// address the caller requested, an identity backend (the trusted-driver profile,
/// Documentation/Architecture/IommuDomain.md ) the physical address. It is pure, bounded and
/// lock-free, and IommuDomain validates its result as it validates a requested address before Map.
/// Competing ownership transitions are internally serialized. After Claim, the owning IommuDomain
/// serializes every other operation. Map, Unmap, Attach, and Detach include the required IOTLB or
/// context invalidation before success is returned. When DrainsDma is true, Unmap success
/// additionally guarantees that every DMA read or write which used the removed translation has
/// completed or been discarded; an implementation uses its architectural drain/completion mechanism
/// and does not trust the unprivileged driver to provide that ordering. Release provides the same
/// completion guarantee when DrainsDma is true for every discarded translation. The one exception
/// is the trusted-driver profile's identity backend (Platform/Device/IdentityDomainBackend), which
/// translates nothing and so cannot drain; a driver on it is trusted with its device's DMA.
/// UnsupportedConfiguration and CapacityExceeded are ordinary, no-write failures. Every other
/// operation error makes native state untrustworthy; the owner retains all authorities until
/// Release succeeds. Release is valid for a healthy or faulted claimed backend and, on success,
/// guarantees that no attached requester can use any prior translation. A failed Claim never
/// transfers ownership or changes native translation state. A failed Release retains ownership and
/// every native resource. Every method is bounded, allocation-free, callback-free, and never enters
/// the owning IommuDomain. Calls are forbidden from an ISR.
class IIommuDomainBackend
{
public:
    [[nodiscard]] virtual bool IsReady() const noexcept = 0;

    /// Translation-only completion requires the core to retain unmapped storage and park it until
    /// trusted device quiescence. Architectural draining backends keep the ordinary immediate
    /// release.
    [[nodiscard]] virtual bool DrainsDma() const noexcept
    {
        return true;
    }

    [[nodiscard]] virtual IommuBackendOwnershipError Claim(const Kernel::Device::IommuDomain& owner,
                                                           Foundation::UInt8 inputAddressBits) noexcept = 0;

    [[nodiscard]] virtual IommuBackendOwnershipError Release(const Kernel::Device::IommuDomain& owner) noexcept = 0;

    [[nodiscard]] virtual Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested,
                        Kernel::Memory::PhysicalAddress physicalStart) const noexcept = 0;

    [[nodiscard]] virtual IommuBackendError Map(const IommuBackendMapRequest& request) noexcept = 0;

    [[nodiscard]] virtual IommuBackendError Unmap(const IommuBackendMapRequest& request) noexcept = 0;

    [[nodiscard]] virtual IommuBackendError Attach(IIommuDevice& device) noexcept = 0;

    [[nodiscard]] virtual IommuBackendError Detach(IIommuDevice& device) noexcept = 0;

    /// Collects the native fault records first; bounded and lock-ordered
    /// like Map.
    [[nodiscard]] virtual IommuBackendFaults GetFaults() noexcept = 0;

protected:
    constexpr IIommuDomainBackend() noexcept = default;
    ~IIommuDomainBackend() noexcept = default;
};

static_assert(sizeof(IommuBackendOwnershipError) == 1U);
static_assert(sizeof(IommuBackendError) == 1U);
static_assert(sizeof(IommuBackendMapRequest) == 32U);
static_assert(sizeof(IommuBackendFaults) == 24U);
static_assert(alignof(IommuBackendMapRequest) == 8U);
} // Namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_IOMMU_DOMAIN_BACKEND_HPP_INCLUDED
