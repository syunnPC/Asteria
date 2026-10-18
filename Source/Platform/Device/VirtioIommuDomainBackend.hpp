// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_VIRTIO_IOMMU_DOMAIN_BACKEND_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_VIRTIO_IOMMU_DOMAIN_BACKEND_HPP_INCLUDED

#include "Hal/Device/IIommuDevice.hpp"
#include "Hal/Device/IIommuDomainBackend.hpp"
#include "Platform/Device/VirtioPciTransport.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Device
{
/// Virtio specifies translation removal but does not explicitly require a
/// physical DMA drain. DrainsDma requires a platform drain of prior DMA;
/// QEMU AHCI caches translations and needs the parked-pin device-quiet drain.
enum class VirtioIommuCompletionPolicy : Foundation::UInt8
{
    TranslationOnly,
    DrainsDma,
};

struct VirtioIommuConfiguration
{
    Foundation::UInt64 PageSizeMask;
    Foundation::UInt64 InputStart;
    Foundation::UInt64 InputEnd;
    Foundation::UInt32 DomainStart;
    Foundation::UInt32 DomainEnd;
    Foundation::UInt32 ProbeSize;
    Foundation::UInt8 Bypass;
};

/// Inclusive input-address extent. The platform supplies all reserved regions,
/// including MSI regions, because PROBE is not negotiated.
struct VirtioIommuReservedRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 End;
};

struct VirtioIommuReservedRangeView
{
    const VirtioIommuReservedRange* Data;
    Foundation::Size Count;
};

class VirtioIommuDomainBackend;

/// One shared device serves all domain backends. No per-domain table pages. Initialize after
/// transport discovery, with six distinct queue pages plus one request and one event page; all
/// eight stay pinned until device reset. The platform serializes lifecycle/configuration and all
/// backend callers across domains, never from an ISR. The shared rank-4 lock additionally
/// serializes competing Claim/Release, requests and fault attribution. It must not be acquired
/// while holding an endpoint lock. All waits are bounded. BYPASS_CONFIG and MAP_UNMAP are required;
/// INPUT_RANGE/DOMAIN_RANGE optional. PROBE is deliberately not negotiated. The reserved-range view
/// is immutable, borrowed for this object's lifetime, and checked before initialization.
/// ClearBypass is called after every initialization, including recovery, at DMA isolation with bus
/// mastering off on every published function. Each (re)initialization clears its flag; no domain
/// may be claimed before it.
class VirtioIommuDevice final
{
public:
    static constexpr Foundation::Size MaximumDomains{16U};
    static constexpr Foundation::Size MaximumAttachments{16U};
    static constexpr Foundation::Size MaximumMappings{64U};
    static constexpr Foundation::UInt16 EventBuffers{8U};

    static constexpr Foundation::Size MaximumReservedRanges{64U};

    explicit VirtioIommuDevice(VirtioPciTransport& transport,
                               const VirtioIommuReservedRangeView reservedRanges) noexcept
        : m_transport{transport}, m_reservedRanges{reservedRanges}
    {
    }
    VirtioIommuDevice(const VirtioIommuDevice&) = delete;
    VirtioIommuDevice& operator=(const VirtioIommuDevice&) = delete;
    [[nodiscard]] VirtioTransportError Initialize(const VirtioDmaPage (&requestQueue)[3U],
                                                  const VirtioDmaPage (&eventQueue)[3U], const VirtioDmaPage& request,
                                                  const VirtioDmaPage& events, Foundation::UInt32 polls,
                                                  VirtioIommuCompletionPolicy completion) noexcept;
    [[nodiscard]] VirtioTransportError ClearBypass() noexcept;
    /// Platform-wide recovery only: every requester is quiesced, bus mastering
    /// is off, and every owning IommuDomain is faulted or being torn down.
    /// Reset removes all device domains; bypass is verified clear before
    /// Release may discard retained authorities. Other domains cannot remain
    /// operational through this reset. No queues are restarted until every
    /// retained owner has successfully released and Initialize is called again.
    [[nodiscard]] VirtioTransportError QuiesceForRecovery() noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept;
    [[nodiscard]] VirtioIommuConfiguration GetConfiguration() const noexcept
    {
        return m_configuration;
    }

private:
    friend class VirtioIommuDomainBackend;
    struct Mapping
    {
        Foundation::UInt64 Start{0U};
        Foundation::UInt64 End{0U};
        Foundation::UInt64 Physical{0U};
        Foundation::UInt32 Flags{0U};
        bool Present{false};
    };
    struct Domain
    {
        const Kernel::Device::IommuDomain* Owner{nullptr};
        Foundation::UInt16 Attached[MaximumAttachments]{};
        Foundation::Size AttachedCount{0U};
        Foundation::UInt8 InputBits{0U};
        Mapping Mappings[MaximumMappings]{};
        Hal::Device::IommuBackendFaults Faults{};
    };
    [[nodiscard]] VirtioTransportError FailInitialize(VirtioTransportError error) noexcept;
    [[nodiscard]] VirtioTransportError ReadConfiguration() noexcept;
    [[nodiscard]] Hal::Device::IommuBackendError Request(Foundation::UInt32 readableBytes) noexcept;
    [[nodiscard]] Hal::Device::IommuBackendError EndpointRequest(Foundation::UInt8 type, Foundation::Size slot,
                                                                 Foundation::UInt16 endpoint) noexcept;
    [[nodiscard]] Hal::Device::IommuBackendError MappingRequest(Foundation::UInt8 type, Foundation::Size slot,
                                                                const Mapping& mapping) noexcept;
    void BeginRequest(Foundation::UInt8 type, Foundation::Size slot) noexcept;
    void DrainFaults() noexcept;

    VirtioPciTransport& m_transport;
    const VirtioIommuReservedRangeView m_reservedRanges;
    mutable Synchronization::SpinLock m_lock;
    VirtioIommuConfiguration m_configuration{};
    VirtioDmaPage m_request{};
    VirtioDmaPage m_events{};
    Domain m_domains[MaximumDomains]{};
    Hal::Device::IommuBackendFaults m_faults{};
    Foundation::UInt32 m_polls{VirtioPciTransport::MaximumPolls};
    VirtioIommuCompletionPolicy m_completion{VirtioIommuCompletionPolicy::TranslationOnly};
    bool m_quiesced{false};
    bool m_initialized{false};
    bool m_bypassCleared{false};
};

/// IIommuDomainBackend is intrinsically single-owner (Map has no domain
/// argument). These small adapters select one ID reserved in the shared
/// VirtioIommuDevice; every domain uses the same device queues and buffers.
/// Map/Unmap without an endpoint update metadata without device requests.
/// Virtio creates domains only on first ATTACH.
/// Successful mappings are retained as bounded metadata so a later first
/// Attach can replay them after the last Detach destroyed the device domain.
/// UNMAP must cover whole MAP extents, as mandated by virtio; splitting an
/// extent is rejected before writing. Successful operations wait for the tail
/// status and device invalidation/drain. Timeout is CommittedStateUnknown;
/// ownership, IDs and DMA storage are retained. Recovery after a poisoned
/// transport requires platform-wide QuiesceForRecovery and reinitialization; a
/// backend Release cannot reset a device shared with other live domains.
class VirtioIommuDomainBackend final : public Hal::Device::IIommuDomainBackend
{
public:
    explicit VirtioIommuDomainBackend(VirtioIommuDevice& device) noexcept : m_device{device}
    {
    }
    VirtioIommuDomainBackend(const VirtioIommuDomainBackend&) = delete;
    VirtioIommuDomainBackend& operator=(const VirtioIommuDomainBackend&) = delete;
    [[nodiscard]] bool IsReady() const noexcept override;
    [[nodiscard]] bool DrainsDma() const noexcept override
    {
        return m_device.m_completion == VirtioIommuCompletionPolicy::DrainsDma;
    }
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
    [[nodiscard]] Foundation::UInt32 GetDomainId() const noexcept;

private:
    [[nodiscard]] bool RangeIsValid(const Hal::Device::IommuBackendMapRequest& request) const noexcept;
    VirtioIommuDevice& m_device;
    Foundation::Size m_slot{VirtioIommuDevice::MaximumDomains};
};
} // Namespace Asteria::Platform::Device
#endif // ASTERIA_PLATFORM_DEVICE_VIRTIO_IOMMU_DOMAIN_BACKEND_HPP_INCLUDED
