// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_BOOT_ENTROPY_GATHERING_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_BOOT_ENTROPY_GATHERING_HPP_INCLUDED

#include "Boot/BootEntropy.hpp"
#include "Boot/Entropy/BootEntropyCollector.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Uefi
{
/// EFI_RNG_PROTOCOL output requested with the firmware's default algorithm.
inline constexpr Foundation::Size FirmwareRngBlockCount{2U};
inline constexpr Foundation::Size FirmwareRngBlockBytes{32U};
/// 64-bit values requested from RDSEED/RDRAND, each one generator block.
inline constexpr Foundation::Size ProcessorRandomValueCount{8U};
/// Cycle-counter readings, one per pool update: twice the deltas the jitter
/// source needs to be credited.
inline constexpr Foundation::Size CycleCounterReadingCount{(2U * Entropy::MinimumCycleCounterDeltas) + 1U};

/// Gathers the boot entropy of this boot and extracts the handoff seed. Mixes, in order: two
/// 32-byte EFI_RNG_PROTOCOL blocks when LocateProtocol finds the protocol; eight RDSEED/RDRAND
/// values where the processor has them; firmware-varying state (GetTime and GetNextMonotonicCount);
/// and CycleCounterReadingCount cycle-counter readings, each taken after the pool update of the
/// previous one. BootEntropyCollector decides which sources are credited. Firmware callbacks: -
/// Before ExitBootServices only: LocateProtocol, the RNG protocol's GetRNG, GetTime and
/// GetNextMonotonicCount, each only when the validated table or protocol provides it. A failing
/// call only withholds that source. - The firmware's RNG driver may allocate; the call must
/// therefore precede the final memory-map transaction. Returns: - The published record, or
/// NoHealthySource when no credited source passed its health tests; the boot must then stop rather
/// than use a predictable seed.
[[nodiscard]] Foundation::Result<BootEntropy, Entropy::BootEntropyError>
GatherBootEntropy(SystemTable& systemTable) noexcept;
} // Namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_BOOT_ENTROPY_GATHERING_HPP_INCLUDED
