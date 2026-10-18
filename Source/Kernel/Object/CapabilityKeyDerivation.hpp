// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_CAPABILITY_KEY_DERIVATION_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_CAPABILITY_KEY_DERIVATION_HPP_INCLUDED

#include "Boot/BootEntropy.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Object
{
/// The 128-bit SipHash-2-4 validation key of one CapabilitySpace, as the two
/// little-endian 64-bit halves the CapabilitySpace constructor takes.
struct CapabilityValidationKey
{
    Foundation::UInt64 Word0;
    Foundation::UInt64 Word1;
};

/// The capability spaces of one boot. Each space derives its key with its
/// own purpose, so no two spaces of a boot share a key; a new space needs a
/// new purpose.
enum class CapabilityKeyPurpose : Foundation::UInt32
{
    /// The boot runtime's space, which holds the initial user image's
    /// capabilities (AMD64 BootRuntime, IA-64 Ia64BspKernelRuntime).
    BootstrapSpace = 1U,
    /// The platform PCI/ECAM runtime's space (Platform::Device::PciEcamRuntime).
    DeviceSpace = 2U,
    /// The boot pager's space (AMD64 Amd64PagerRuntime).
    PagerSpace = 3U,
    /// The kernel-only space in which the pager channel creates each
    /// FaultToken before delivery moves it to the pager (PagerChannel).
    FaultTokenSpace = 4U,
    /// The AMD64 test firmware's spinner space (Amd64SpinnerRuntime).
    SpinnerSpace = 5U,
    /// One per slot of Object::CapabilitySpacePool for user-created CapabilitySpaces.
    RuntimeSpace0 = 6U,
    RuntimeSpace1 = 7U,
    RuntimeSpace2 = 8U,
    RuntimeSpace3 = 9U,
    RuntimeSpace4 = 10U,
    RuntimeSpace5 = 11U,
    RuntimeSpace6 = 12U,
    RuntimeSpace7 = 13U,
    RuntimeSpace8 = 14U,
    RuntimeSpace9 = 15U,
    RuntimeSpace10 = 16U,
    RuntimeSpace11 = 17U,
    RuntimeSpace12 = 18U,
    RuntimeSpace13 = 19U,
    RuntimeSpace14 = 20U,
    RuntimeSpace15 = 21U,
};

enum class CapabilityKeyError : Foundation::UInt32
{
    None,
    InvalidPurpose,
    /// The record names no credited source, an unknown source, or has a
    /// nonzero reserved field: the boot application never published it.
    MissingEntropy,
    /// Every seed byte has the same value (for example, all zero).
    DegenerateSeed,
};

/// Derives the validation key of the capability space `purpose` from the
/// boot seed: the first 16 bytes of HKDF-SHA256-Expand (RFC 5869) with the
/// seed as pseudorandom key and info "Asteria capability validation key v1"
/// followed by the purpose as a little-endian 32-bit value; bytes 0-7 form
/// Word0 and bytes 8-15 Word1, little-endian.
/// Failure:
/// - Rejects an unpublished or degenerate record instead of deriving a
/// predictable key; a boot runtime that receives an error must not
/// construct its capability space.
/// Execution:
/// - Pure and bounded; no allocation, no lock, no firmware call.
[[nodiscard]] Foundation::Result<CapabilityValidationKey, CapabilityKeyError>
DeriveCapabilityValidationKey(const Boot::BootEntropy& entropy, CapabilityKeyPurpose purpose) noexcept;

static_assert(sizeof(CapabilityValidationKey) == 16U);
static_assert(sizeof(CapabilityKeyPurpose) == 4U);
static_assert(sizeof(CapabilityKeyError) == 4U);
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_CAPABILITY_KEY_DERIVATION_HPP_INCLUDED
