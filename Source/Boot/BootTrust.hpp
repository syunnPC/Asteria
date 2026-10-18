// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_TRUST_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_TRUST_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
/// The trusted-driver allowlist of BOOT.CFG. An entry names a PCI function by address and/or
/// vendor:device, the driver that may drive it without DMA isolation, the boot module that carries
/// that driver and the SHA-256 digest that module must have. It never applies by itself: the kernel
/// applies a verified entry only to a published function no usable IOMMU serves.
constexpr Foundation::Size BootTrustMaximumEntries{8U};
constexpr Foundation::Size BootTrustDigestBytes{32U};

/// Selector bits: which of the function's identities an entry names (at least one).
constexpr Foundation::UInt8 BootTrustSelectsAddress{1U};
constexpr Foundation::UInt8 BootTrustSelectsIdentifier{2U};

/// The drivers an entry may name: the DMA drivers of the component module.
enum class BootTrustDriver : Foundation::UInt8
{
    None,
    Ahci,
    E1000e,
    Xhci,
    /// The USB host group driver of EHCI, OHCI and UHCI functions.
    UsbHost,
};

/// What the loader found when it hashed the named module.
enum class BootTrustVerdict : Foundation::UInt8
{
    Unchecked,
    Verified,
    DigestMismatch,
};

struct BootTrustEntry
{
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt8 Selector;
    Foundation::UInt16 Vendor;
    Foundation::UInt16 Product;
    BootTrustDriver Driver;
    BootTrustVerdict Verdict;
    /// Index into BootFileRegion::Modules.
    Foundation::UInt8 Module;
    Foundation::UInt8 Reserved[3];
    Foundation::UInt8 Digest[BootTrustDigestBytes];
};

/// Plain data, copied into BootInfo and covered by its CRC.
struct BootTrustPolicy
{
    Foundation::UInt64 Count;
    BootTrustEntry Entries[BootTrustMaximumEntries];
};

/// The manifest's driver name for driver ("ahci", "e1000e", "xhci", "usb"); "none" otherwise.
[[nodiscard]] constexpr const char* BootTrustDriverName(const BootTrustDriver driver) noexcept
{
    switch (driver)
    {
    case BootTrustDriver::Ahci:
        return "ahci";
    case BootTrustDriver::E1000e:
        return "e1000e";
    case BootTrustDriver::Xhci:
        return "xhci";
    case BootTrustDriver::UsbHost:
        return "usb";
    case BootTrustDriver::None:
        break;
    }
    return "none";
}

static_assert(__builtin_offsetof(BootTrustEntry, Selector) == 5U);
static_assert(__builtin_offsetof(BootTrustEntry, Vendor) == 6U);
static_assert(__builtin_offsetof(BootTrustEntry, Driver) == 10U);
static_assert(__builtin_offsetof(BootTrustEntry, Module) == 12U);
static_assert(__builtin_offsetof(BootTrustEntry, Digest) == 16U);
static_assert(sizeof(BootTrustEntry) == 48U);
static_assert(sizeof(BootTrustPolicy) == 392U);
} // Namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_TRUST_HPP_INCLUDED
