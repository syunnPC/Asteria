// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_VTD_DOMAIN_GROUP_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_VTD_DOMAIN_GROUP_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Platform/Device/VtdDomainBackend.hpp"

namespace Asteria::Platform::Device
{
/// One user domain across the enabled units. Each unit owns independent
/// tables and capabilities; maps are replicated, attachments route by the
/// unit's resolved requester list. The pool serializes Claim/Release and
/// the owning IommuDomain serializes the other calls, as for a single backend.
/// An ownership lock (rank 4) serializes Claim/Release; ordinary operations
/// use the owning domain's serialization. Backends rank 5 and units rank 6.
/// Failure after any hardware write is CommittedStateUnknown, retaining the owner's pins for its drain.
class VtdDomainGroup final : public Hal::Device::IIommuDomainBackend
{
public:
    static constexpr Foundation::Size MaximumUnits{Boot::Acpi::MaximumAcpiDmaRemappingUnits};
    VtdDomainGroup(VtdDomainBackend* const (&backends)[MaximumUnits], VtdUnit* const (&units)[MaximumUnits],
                   Foundation::Size count) noexcept;
    VtdDomainGroup(const VtdDomainGroup&) = delete;
    VtdDomainGroup& operator=(const VtdDomainGroup&) = delete;

    [[nodiscard]] bool IsReady() const noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendOwnershipError Claim(const Kernel::Device::IommuDomain& owner,
                                                                Foundation::UInt8 inputAddressBits) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendOwnershipError
    Release(const Kernel::Device::IommuDomain& owner) noexcept override;
    /// A translating backend: the device address is the one requested.
    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested, Kernel::Memory::PhysicalAddress) const noexcept override
    {
        return requested;
    }
    [[nodiscard]] Hal::Device::IommuBackendError
    Map(const Hal::Device::IommuBackendMapRequest& request) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendError
    Unmap(const Hal::Device::IommuBackendMapRequest& request) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendError Attach(Hal::Device::IIommuDevice& device) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendError Detach(Hal::Device::IIommuDevice& device) noexcept override;
    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override;

private:
    [[nodiscard]] VtdDomainBackend* Route(Hal::Device::IIommuDevice& device) const noexcept;
    VtdDomainBackend* m_backends[MaximumUnits]{};
    VtdUnit* m_units[MaximumUnits]{};
    Foundation::Size m_count{0U};
    bool m_claimed[MaximumUnits]{};
    const Kernel::Device::IommuDomain* m_owner{nullptr};
    Platform::Synchronization::SpinLock m_ownership;
};
} // namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_VTD_DOMAIN_GROUP_HPP_INCLUDED
