// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_ENTROPY_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_ENTROPY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
inline constexpr Foundation::Size BootEntropySeedBytes{32U};

/// One bit of BootEntropy::Sources per kind of boot entropy source.
enum class BootEntropySource : Foundation::UInt32
{
    /// EFI_RNG_PROTOCOL output (the firmware's default algorithm).
    FirmwareRng = 1U << 0U,
    /// RDSEED, else RDRAND, where CPUID reports them (AMD64 only).
    ProcessorRng = 1U << 1U,
    /// Timing jitter of the processor's cycle counter (TSC or ITC).
    CycleCounterJitter = 1U << 2U,
    /// Firmware-varying state such as the time of day; mixed, never credited.
    FirmwareState = 1U << 3U,
};

[[nodiscard]] constexpr Foundation::UInt32 GetBootEntropySourceMask(const BootEntropySource source) noexcept
{
    return static_cast<Foundation::UInt32>(source);
}

/// Sources that can make a seed acceptable on their own.
inline constexpr Foundation::UInt32 CreditedBootEntropySources{
    GetBootEntropySourceMask(BootEntropySource::FirmwareRng) |
    GetBootEntropySourceMask(BootEntropySource::ProcessorRng) |
    GetBootEntropySourceMask(BootEntropySource::CycleCounterJitter)};
inline constexpr Foundation::UInt32 KnownBootEntropySources{CreditedBootEntropySources |
                                                            GetBootEntropySourceMask(BootEntropySource::FirmwareState)};

/// The boot seed of the handoff (BootInfo::Entropy).
///
/// Seed is the HKDF-SHA256 extraction (RFC 5869) of every sample the UEFI
/// application gathered before ExitBootServices (BootEntropyCollector).
/// Sources records the credited sources whose samples passed their health
/// tests, plus FirmwareState when firmware-varying state was mixed; a
/// published record has at least one credited source and Reserved zero. The
/// seed is secret: it never leaves kernel memory, and the kernel derives the
/// capability-space validation keys from it (CapabilityKeyDerivation).
struct BootEntropy
{
    Foundation::UInt8 Seed[BootEntropySeedBytes];
    Foundation::UInt32 Sources;
    Foundation::UInt32 Reserved;
};

enum class BootEntropyState : Foundation::UInt32
{
    /// At least one credited source, only known source bits, a zero
    /// reserved field, and a seed that is not one repeated byte.
    Published,
    /// No credited source, an unknown source bit, or a nonzero reserved
    /// field: the record was never published (a reset BootInfo is all zero).
    Unpublished,
    /// Every seed byte has the same value.
    DegenerateSeed,
};

/// Classifies a handoff record; only a Published record may key anything.
[[nodiscard]] constexpr BootEntropyState GetBootEntropyState(const BootEntropy& entropy) noexcept
{
    if (((entropy.Sources & CreditedBootEntropySources) == 0U) ||
        ((entropy.Sources & ~KnownBootEntropySources) != 0U) || (entropy.Reserved != 0U))
    {
        return BootEntropyState::Unpublished;
    }
    for (Foundation::Size index{1U}; index < BootEntropySeedBytes; ++index)
    {
        if (entropy.Seed[index] != entropy.Seed[0U])
        {
            return BootEntropyState::Published;
        }
    }
    return BootEntropyState::DegenerateSeed;
}

static_assert(sizeof(BootEntropySource) == 4U);
static_assert(sizeof(BootEntropyState) == 4U);
static_assert(sizeof(BootEntropy) == 40U);
static_assert(alignof(BootEntropy) == 4U);
static_assert(__builtin_offsetof(BootEntropy, Sources) == 32U);
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_ENTROPY_HPP_INCLUDED
