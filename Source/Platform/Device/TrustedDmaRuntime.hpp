// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_TRUSTED_DMA_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_TRUSTED_DMA_RUNTIME_HPP_INCLUDED

#include "Boot/BootFiles.hpp"
#include "Boot/BootTrust.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Device/TrustedDmaPolicy.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Device/IdentityDomainBackend.hpp"
#include "Kernel/Device/IommuDomainPool.hpp"

namespace Asteria::Platform::Device
{
/// One published function a DMA driver serves: its registry identity and the driver the root
/// service runs for its device slot (Abi::PagerDevice*).
struct TrustedDmaCandidate
{
    Kernel::Object::ObjectId Function;
    Boot::BootTrustDriver Driver;
    Foundation::UInt8 Reserved[7];
};

/// Applies the BOOT.CFG trust allowlist once the IOMMU takeover decided what the platform has, the
/// same way on both ISAs. For each candidate it takes Kernel::Device::DecideTrustedDma's outcome;
/// an Applied one grants the PciFunction trusted DMA (PciFunction::GrantTrustedDma). It copies the
/// entries into the device inventory with each one's outcome, marks the trusted functions there
/// (the root service's `# devices:` lines say "TRUSTED: no DMA isolation") and reports every
/// decision on the debug channel ("ASTERIA:DMA-TRUST:..."); a policy without entries reports
/// nothing and changes nothing. Its four identity backends serve the IommuDomain pool, in the
/// platform's own slots (which no IOMMU backend uses then), when at least one function is trusted
/// and no IOMMU backend configured it. Boot only, BSP, before the pager is admitted and before any
/// assignment; no allocation; pins each candidate through the registry for the call.
class TrustedDmaRuntime final
{
public:
    constexpr TrustedDmaRuntime() noexcept = default;

    TrustedDmaRuntime(const TrustedDmaRuntime&) = delete;
    TrustedDmaRuntime& operator=(const TrustedDmaRuntime&) = delete;

    /// Decides and applies; the number of functions granted trusted DMA.
    [[nodiscard]] Foundation::Size Apply(Kernel::Object::ObjectRegistry& registry, const Boot::BootTrustPolicy& policy,
                                         const Boot::BootFileRegion& files,
                                         Kernel::Device::IommuAvailability availability,
                                         const TrustedDmaCandidate* candidates, Foundation::Size count) noexcept;

    /// The functions Apply granted trusted DMA.
    [[nodiscard]] Foundation::Size GetTrustedCount() const noexcept
    {
        return m_trusted;
    }

    /// The pool's backends: the identity backends, whatever Apply decided.
    [[nodiscard]] Hal::Device::IIommuDomainBackend* const (
        &GetBackends() noexcept)[Kernel::Device::IommuDomainPool::Capacity];

private:
    IdentityDomainBackend m_identity[Kernel::Device::IommuDomainPool::Capacity]{};
    Hal::Device::IIommuDomainBackend* m_backends[Kernel::Device::IommuDomainPool::Capacity]{};
    Foundation::Size m_trusted{0U};
};
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_TRUSTED_DMA_RUNTIME_HPP_INCLUDED
