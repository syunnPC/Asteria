// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_IMAGE_RELOCATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_IMAGE_RELOCATION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
enum class Ia64ImageRelocationError : Foundation::UInt32
{
    None,
    InvalidImage,
    InvalidDestination,
    InvalidRelocations,
    InvalidEntryDescriptor,
};

struct Ia64ImageRelocationResult
{
    Foundation::UInt64 EntryDescriptor;
};

/// Copies a canonical, firmware-relocated PE image into disjoint caller-owned
/// writable storage at the 16 MiB placement alignment. Applies only the
/// normalizer's ordered DIR64/IMM64 fixups, using destination minus actual
/// source base (unsigned modulo 2^64, including downward relocation).
/// descriptorRva selects a continuation descriptor in.opd; zero selects the
/// PE entry. All rejection precedes writes. Source and destination must be
/// stable identity aliases with no other alias onto either range. This owns
/// no allocation, touches no mutable global and executes no privileged code.
/// The caller must make the successful copy instruction-coherent before entry.
class Ia64ImageRelocation final
{
public:
    [[nodiscard]] static Foundation::Result<Ia64ImageRelocationResult, Ia64ImageRelocationError>
    RelocateToAlignment(const void* image, Foundation::Size imageBytes, void* destination,
                        Foundation::Size destinationBytes, Foundation::UInt32 descriptorRva = 0U) noexcept;

private:
    Ia64ImageRelocation() = delete;
};

/// Addresses materialized from this executing image's relocated linker symbols.
/// Available only on IA-64 firmware; never consult EFI_LOADED_IMAGE.
[[nodiscard]] Foundation::UInt64 GetIa64ExecutingImageBase() noexcept;
[[nodiscard]] Foundation::UInt64 GetIa64ExecutingImageEnd() noexcept;
[[nodiscard]] Foundation::UInt64 GetIa64RelocatedEntryDescriptor() noexcept;

/// Firmware-only cache publication; the hosted adapter performs no operation.
/// image must be bundle-aligned; bytes must be positive, a multiple of 16,
/// and describe a nonwrapping writable identity range. No allocation/blocking.
void MakeIa64ImageInstructionCoherent(void* image, Foundation::Size bytes) noexcept;
} // Namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_IMAGE_RELOCATION_HPP_INCLUDED
