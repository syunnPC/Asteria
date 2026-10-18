// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_PCI_BOOTSTRAP_BINDING_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_PCI_BOOTSTRAP_BINDING_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/AhciHbaRegisterSnapshotReader.hpp"
#include "Kernel/Device/PciEndpointBarDecoder.hpp"
#include "Kernel/Device/PciFunctionCatalog.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Device
{
enum class AhciPciBootstrapBindingState : Foundation::UInt8
{
    Empty,
    Ready,
};

enum class AhciPciBootstrapBindingError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    CatalogNotReady,
    CatalogQueryRejected,
    NoAhciController,
    AmbiguousAhciController,
    AbarDecodeRejected,
    AbarKindRejected,
    AbarPrefetchable,
    SnapshotReadRejected,
};

enum class AhciPciBootstrapBindingQueryError : Foundation::UInt8
{
    None,
    NotReady,
};

struct AhciPciBootstrapBindingRequest
{
    /// Inclusive maximum implemented physical address.
    Foundation::UInt64 MaximumPhysicalAddress;
    /// Exact authoritative BAR5 aperture extent supplied by the resource
    /// authority that established the mapped-MMIO reader's access.
    Foundation::UInt64 AbarExtentBytes;
};

struct AhciPciBootstrapBindingResult
{
    PciFunctionDescriptor Function;
    PciEndpointBar Abar;
    AhciHbaRegisterSnapshot Snapshot;
};

struct AhciPciBootstrapBindingFailure
{
    PciFunctionCatalogFailure Catalog;
    PciEndpointBarDecodeFailure Abar;
    AhciHbaSnapshotFailure Snapshot;
    Foundation::Size MatchingFunctionCount;
    AhciPciBootstrapBindingError Error;
    Foundation::UInt8 Reserved[7];
};

/// Selects exactly one standards-defined AHCI PCI endpoint, decodes BAR5, and
/// captures its initial read-only HBA register snapshot as one publication.
///
/// Selection requires the mass-storage class (01h), SATA subclass (06h), AHCI
/// programming interface (01h), and a conventional type-zero endpoint header.
/// Zero matches and multiple matches are distinct deterministic failures; no
/// controller is selected by enumeration accident.
///
/// The caller supplies the exact authoritative ABAR extent. Unknown or inferred
/// extents are not accepted by this contract. BAR5 must decode as non-prefetchable
/// 32-bit MMIO. The binding performs no PCI or MMIO writes and does not size a
/// BAR, map memory, enable bus mastering, configure DMA, start a port, or touch
/// interrupts.
///
/// A successful Prepare publishes Function, Abar, and Snapshot together. The
/// publication is immutable thereafter; GetResult never exposes partial state
/// and leaves its output unchanged before publication. A failed Prepare does
/// not publish and may be retried. Calls are internally serialized, may spin,
/// and are forbidden from ISR context and same-CPU reentry.
///
/// The catalog, decoder, reader, and all of their backends and access authority
/// are non-owning and must outlive this object and every operation. The catalog
/// must remain immutable after discovery. The mapped-MMIO authority must cover
/// the decoded ABAR range for the complete Prepare call.
class AhciPciBootstrapBinding final
{
public:
    constexpr AhciPciBootstrapBinding(const PciFunctionCatalog& catalog, PciEndpointBarDecoder& barDecoder,
                                      AhciHbaRegisterSnapshotReader& snapshotReader) noexcept
        : m_catalog{catalog}, m_barDecoder{barDecoder}, m_snapshotReader{snapshotReader}
    {
    }

    AhciPciBootstrapBinding(const AhciPciBootstrapBinding&) = delete;
    AhciPciBootstrapBinding& operator=(const AhciPciBootstrapBinding&) = delete;
    AhciPciBootstrapBinding(AhciPciBootstrapBinding&&) = delete;
    AhciPciBootstrapBinding& operator=(AhciPciBootstrapBinding&&) = delete;

    [[nodiscard]] AhciPciBootstrapBindingFailure Prepare(AhciPciBootstrapBindingRequest request) noexcept;

    /// Copies the complete immutable publication. Failure leaves output
    /// unchanged.
    [[nodiscard]] AhciPciBootstrapBindingQueryError GetResult(AhciPciBootstrapBindingResult& output) const noexcept;

    [[nodiscard]] AhciPciBootstrapBindingState GetState() const noexcept;

    /// Shared AHCI classification for bootstrap binding and DMA lifetime policy.
    [[nodiscard]] static bool IsAhciEndpoint(const PciFunctionDescriptor& descriptor) noexcept;

private:
    [[nodiscard]] static AhciPciBootstrapBindingFailure MakeFailure(AhciPciBootstrapBindingError error,
                                                                    Foundation::Size matchingFunctionCount = 0U,
                                                                    PciFunctionCatalogFailure catalog = {},
                                                                    PciEndpointBarDecodeFailure abar = {},
                                                                    AhciHbaSnapshotFailure snapshot = {}) noexcept;
    [[nodiscard]] AhciPciBootstrapBindingError
    SelectAhciEndpoint(PciFunctionDescriptor& output, Foundation::Size& matchingFunctionCount,
                       PciFunctionCatalogFailure& catalogFailure) const noexcept;

    const PciFunctionCatalog& m_catalog;
    PciEndpointBarDecoder& m_barDecoder;
    AhciHbaRegisterSnapshotReader& m_snapshotReader;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    AhciPciBootstrapBindingResult m_result{};
    AhciPciBootstrapBindingState m_state{AhciPciBootstrapBindingState::Empty};
};

static_assert(sizeof(AhciPciBootstrapBindingState) == 1U);
static_assert(sizeof(AhciPciBootstrapBindingError) == 1U);
static_assert(sizeof(AhciPciBootstrapBindingQueryError) == 1U);
static_assert(sizeof(AhciPciBootstrapBindingRequest) == 16U);
static_assert(alignof(AhciPciBootstrapBindingRequest) == 8U);
static_assert(sizeof(AhciPciBootstrapBindingResult) == 80U);
static_assert(alignof(AhciPciBootstrapBindingResult) == 8U);
static_assert(sizeof(AhciPciBootstrapBindingFailure) == 48U);
static_assert(alignof(AhciPciBootstrapBindingFailure) == 8U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_PCI_BOOTSTRAP_BINDING_HPP_INCLUDED
