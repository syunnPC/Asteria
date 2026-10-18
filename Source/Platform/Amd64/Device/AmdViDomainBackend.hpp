// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_DOMAIN_BACKEND_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_DOMAIN_BACKEND_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IIommuDevice.hpp"
#include "Hal/Device/IIommuDomainBackend.hpp"
#include "Platform/Amd64/Device/AmdViUnit.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Amd64::Device
{
/// The AMD-Vi translation of one IommuDomain: a domain identifier of its unit and eight v1 I/O
/// table pages (64 for boot-owned firmware domains), the top one allocated at Claim and the others
/// as mappings need them. Tables are not returned until Release, so a domain that keeps mapping
/// fresh 2 MiB windows exhausts them (CapacityExceeded, before anything is written). Map
/// invalidates negative translations (NPCache); Unmap invalidates the domain's IOTLB with draining.
/// Release blocks every attached requester again, invalidates, and zeroes the pages. The owning
/// IommuDomain serializes every call but Claim and Release, which this backend's lock (rank 5,
/// acquiring the unit's) serializes.
class AmdViDomainBackend final : public Hal::Device::IIommuDomainBackend
{
public:
    static constexpr Foundation::Size TablePages{8U};
    static constexpr Foundation::Size MaximumTablePages{64U};
    static constexpr Foundation::Size MaximumAttachments{16U};

    AmdViDomainBackend(AmdViUnit& unit, Foundation::UInt16 domain, const VtdTablePage (&pages)[TablePages]) noexcept;

    /// Larger boot-owned firmware table reservation; pages/count remain bounded.
    AmdViDomainBackend(AmdViUnit& unit, Foundation::UInt16 domain, const VtdTablePage* pages,
                       Foundation::Size count) noexcept;

    AmdViDomainBackend(const AmdViDomainBackend&) = delete;
    AmdViDomainBackend& operator=(const AmdViDomainBackend&) = delete;

    /// Preflight without writes; owner serializes it with Map.
    [[nodiscard]] Hal::Device::IommuBackendError
    CheckMap(const Hal::Device::IommuBackendMapRequest& request) const noexcept;
    /// Rolls back only a Claim with no maps or attachments (group preflight).
    [[nodiscard]] bool CancelClaim(const Kernel::Device::IommuDomain& owner) noexcept;
    /// Boot-only persistent firmware domain, never claimed by user code.
    [[nodiscard]] bool PrepareReserved() noexcept;
    [[nodiscard]] Hal::Device::IommuBackendError MapReserved(Foundation::UInt64 base, Foundation::UInt64 limit,
                                                             Kernel::Device::DmaPermissions permissions) noexcept;
    [[nodiscard]] VtdUnitError SeedReserved(Foundation::UInt16 requester) noexcept;

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
    /// Polls the unit, then reports the faults of this domain identifier.
    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override;

    [[nodiscard]] Foundation::UInt16 GetDomain() const noexcept
    {
        return m_domain;
    }

    /// Table pages in use, the top one included; zero while unclaimed.
    [[nodiscard]] Foundation::Size GetUsedPages() const noexcept;

private:
    [[nodiscard]] bool RangeIsValid(const Hal::Device::IommuBackendMapRequest& request) const noexcept;
    [[nodiscard]] Foundation::Size CountMissingTables(Foundation::UInt64 start,
                                                      Foundation::UInt64 pages) const noexcept;
    [[nodiscard]] volatile Foundation::UInt64* FindTable(Foundation::UInt64 physicalAddress) const noexcept;
    [[nodiscard]] volatile Foundation::UInt64* Descend(volatile Foundation::UInt64* table, Foundation::Size index,
                                                       bool allocate, Foundation::UInt8 nextLevel) noexcept;
    [[nodiscard]] volatile Foundation::UInt64* FindLeaf(Foundation::UInt64 address, bool allocate) noexcept;
    void ZeroPages() noexcept;

    AmdViUnit& m_unit;
    const Foundation::UInt16 m_domain;
    VtdTablePage m_pages[MaximumTablePages]{};
    Foundation::Size m_pageCount{0U};
    mutable Platform::Synchronization::SpinLock m_synchronization;
    const Kernel::Device::IommuDomain* m_owner{nullptr};
    Foundation::UInt16 m_attached[MaximumAttachments]{};
    Foundation::Size m_attachedCount{0U};
    Foundation::Size m_usedPages{0U};
    Foundation::UInt8 m_levels{0U};
    Foundation::UInt8 m_inputAddressBits{0U};
    bool m_reserved{false};
};
} // Namespace Asteria::Platform::Amd64::Device

#endif // ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_DOMAIN_BACKEND_HPP_INCLUDED
