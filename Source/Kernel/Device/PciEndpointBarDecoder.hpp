// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_ENDPOINT_BAR_DECODER_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_ENDPOINT_BAR_DECODER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Kernel::Device
{
enum class PciEndpointBarKind : Foundation::UInt8
{
    Io,
    Memory32,
    Memory64,
};

enum class PciEndpointBarExtentState : Foundation::UInt8
{
    Unknown,
    Authoritative,
};

enum class PciEndpointBarDecodeError : Foundation::UInt8
{
    None,
    InvalidFunctionAddress,
    InvalidBarIndex,
    InvalidExtent,
    ConfigurationReadFailed,
    NoAssignedAddress,
    ReservedEncoding,
    MissingUpperBar,
    PhysicalAddressLimitExceeded,
};

struct PciEndpointBarDecodeRequest
{
    /// Inclusive maximum implemented physical address. Ignored for I/O BARs.
    Foundation::UInt64 MaximumPhysicalAddress;
    /// Zero when ExtentState is Unknown; otherwise the exact authoritative
    /// aperture extent. The decoder never probes or infers this value.
    Foundation::UInt64 ExtentBytes;
    PciFunctionAddress Address;
    PciEndpointBarExtentState ExtentState;
    Foundation::UInt8 BarIndex;
};

struct PciEndpointBar
{
    Foundation::UInt64 BaseAddress;
    /// Zero precisely when ExtentState is Unknown.
    Foundation::UInt64 ExtentBytes;
    PciEndpointBarKind Kind;
    PciEndpointBarExtentState ExtentState;
    Foundation::UInt8 BarIndex;
    /// One for an I/O or 32-bit memory BAR, two for a 64-bit memory BAR.
    Foundation::UInt8 RegisterCount;
    bool Prefetchable;
    Foundation::UInt8 Reserved[3];
};

struct PciEndpointBarDecodeFailure
{
    PciEndpointBarDecodeError Error;
    Hal::Device::PciConfigurationReadError ConfigurationRead;
    Foundation::UInt8 Reserved[6];
};

/// Decodes one conventional endpoint BAR without probing or writing PCI state. Preconditions: - The
/// caller has verified that Address names a type-zero endpoint header; only that layout defines all
/// six BAR registers used by this API. The requested BarIndex names the first register of a BAR. A
/// caller walking the BAR array advances by the successful result's RegisterCount. With the
/// read-only PCI configuration contract, a zero BAR cannot be distinguished as unimplemented versus
/// implemented but unassigned; both are reported as NoAssignedAddress. Extent remains Unknown
/// unless the caller supplies an exact extent obtained from a separate resource authority. Calls
/// are bounded, allocation-free, callback-free, and perform at most two configuration reads.
/// Failure leaves output unchanged. The supplied PCI configuration backend is non-owning and must
/// outlive this decoder and every Decode call. This API inherits the backend's prohibition on calls
/// from an ISR or other interrupt context and on same-CPU reentry. For a 64-bit BAR, the caller
/// must keep PCI configuration state stable across the lower and upper register reads; backend
/// serialization of individual reads does not make the pair an atomic snapshot.
class PciEndpointBarDecoder final
{
public:
    explicit constexpr PciEndpointBarDecoder(Hal::Device::IPciConfigurationSpace& configurationSpace) noexcept
        : m_configurationSpace{configurationSpace}
    {
    }

    [[nodiscard]] PciEndpointBarDecodeFailure Decode(const PciEndpointBarDecodeRequest& request,
                                                     PciEndpointBar& output) noexcept;

private:
    [[nodiscard]] static PciEndpointBarDecodeFailure
    MakeFailure(PciEndpointBarDecodeError error, Hal::Device::PciConfigurationReadError configurationRead =
                                                     Hal::Device::PciConfigurationReadError::None) noexcept;

    Hal::Device::IPciConfigurationSpace& m_configurationSpace;
};

static_assert(sizeof(PciEndpointBarKind) == 1U);
static_assert(sizeof(PciEndpointBarExtentState) == 1U);
static_assert(sizeof(PciEndpointBarDecodeError) == 1U);
static_assert(sizeof(PciEndpointBarDecodeRequest) == 24U);
static_assert(alignof(PciEndpointBarDecodeRequest) == 8U);
static_assert(sizeof(PciEndpointBar) == 24U);
static_assert(alignof(PciEndpointBar) == 8U);
static_assert(sizeof(PciEndpointBarDecodeFailure) == 8U);
static_assert(alignof(PciEndpointBarDecodeFailure) == 1U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_ENDPOINT_BAR_DECODER_HPP_INCLUDED
