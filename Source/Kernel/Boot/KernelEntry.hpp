// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_BOOT_KERNEL_ENTRY_HPP_INCLUDED
#define ASTERIA_KERNEL_BOOT_KERNEL_ENTRY_HPP_INCLUDED

#include "Boot/BootInfo.hpp"

#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Amd64/Memory/InitialPagingCapabilityCapture.hpp"
#endif

namespace Asteria::Boot::Bootstrap
{
/// Read-only address query for the single-image loader. Preparation initializes
/// this kernel-owned record; the loader only publishes its address.
[[nodiscard]] Foundation::UInt64 GetKernelEvidenceAddress() noexcept;

/// Imports a sealed, identity-addressed BootInfo into kernel-owned storage.
/// Single bootstrap CPU, before ExitBootServices. Calls no firmware and changes
/// no firmware allocation. A rejection leaves firmware error reporting usable.
/// Success returns the timer/reservation receipt and reseals both copies.
[[nodiscard]] bool PrepareKernel(BootInfo& info) noexcept;

/// Reimports the final sealed receipt after ExitBootServices, retaining no
/// pointer to the loader's record. Failure stops through the native diagnostic.
[[noreturn]] void EnterKernel(const BootInfo& info) noexcept;

#if defined(ASTERIA_TARGET_AMD64)
/// Read-only failure facts for the loader's console report. Valid after a
/// preparation call; no allocation, blocking or firmware callbacks.
[[nodiscard]] bool IsAmd64TableRejection() noexcept;
[[nodiscard]] const char* GetAmd64FinalMemoryMapRejection() noexcept;
[[nodiscard]] Platform::Amd64::Memory::InitialPagingCapabilityCaptureError GetAmd64PagingCapabilityError() noexcept;
[[nodiscard]] Platform::Amd64::Memory::InitialProcessorStateCaptureError GetAmd64ProcessorStateCaptureError() noexcept;
[[nodiscard]] const Platform::Amd64::Memory::InitialAddressSpacePreparation& GetAmd64AddressSpacePreparation() noexcept;
[[nodiscard]] const Platform::Amd64::Memory::InitialProcessorStateReport& GetAmd64ProcessorStateReport() noexcept;
#endif
} // namespace Asteria::Boot::Bootstrap

#endif // ASTERIA_KERNEL_BOOT_KERNEL_ENTRY_HPP_INCLUDED
