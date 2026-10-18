// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_TRUSTED_DMA_POLICY_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_TRUSTED_DMA_POLICY_HPP_INCLUDED

#include "Boot/BootTrust.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Kernel::Device
{
/// What DMA protection the platform has for a published function once its IOMMU takeover ended:
/// Working when a backend configured IommuDomains (VT-d, AMD-Vi, virtio-iommu); Refused when the
/// firmware describes an IOMMU the boot could not use (a refused topology, handoff, configuration
/// or enable); Absent when nothing describes one.
enum class IommuAvailability : Foundation::UInt8
{
    Absent,
    Refused,
    Working,
};

/// One published function a DMA driver serves, as the decision sees it.
struct TrustedDmaFunction
{
    PciFunctionAddress Address;
    Foundation::UInt16 Vendor;
    Foundation::UInt16 Product;
    /// The driver the root service runs for the function's device slot.
    Boot::BootTrustDriver Driver;
    Foundation::UInt8 Reserved;
};

enum class TrustedDmaOutcome : Foundation::UInt8
{
    /// No entry names the function: DMA isolation is required.
    NoEntry,
    /// The function is driven without
    Applied,
    /// An IOMMU confines the function; its entry is unused.
    IommuConfines,
    /// The entry's module digest did not verify.
    DigestMismatch,
    /// The entry names another driver than the one the function has.
    DriverMismatch,
    /// More than one entry names the function; none applies.
    Ambiguous,
};

struct TrustedDmaDecision
{
    TrustedDmaOutcome Outcome;
    /// One-based number of the entry the outcome is about; zero for NoEntry
    /// and Ambiguous.
    Foundation::UInt8 Entry;
};

/// Whether entry names function: its address where the entry selects by
/// address, its vendor:device where it selects by identifier, both where it
/// selects by both; an entry without a selector names nothing.
[[nodiscard]] bool TrustEntryMatches(const Boot::BootTrustEntry& entry, const TrustedDmaFunction& function) noexcept;

/// The trusted-driver decision for one published function: NoEntry unless an
/// entry names it; Ambiguous when several do; then, in order, DriverMismatch,
/// DigestMismatch (any verdict but Verified), IommuConfines when the IOMMU is
/// Working, and Applied otherwise (Absent or Refused). Pure, bounded by the
/// entry count; any context.
[[nodiscard]] TrustedDmaDecision DecideTrustedDma(const Boot::BootTrustPolicy& policy,
                                                  const TrustedDmaFunction& function,
                                                  IommuAvailability availability) noexcept;
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_TRUSTED_DMA_POLICY_HPP_INCLUDED
