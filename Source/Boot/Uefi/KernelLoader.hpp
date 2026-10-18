// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_KERNEL_LOADER_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_KERNEL_LOADER_HPP_INCLUDED

#include "Boot/KernelElf.hpp"
#include "Boot/KernelEntryAbi.hpp"
#include "Boot/Uefi/UefiTypes.hpp"

namespace Asteria::Boot::Uefi
{
/// Single BSP, before the final memory-map transaction. Preflight and
/// relocation precede publication of all three entry addresses. Allocations
/// come solely from the kernel note; no kernel reservation constant belongs
/// in this adapter. Failure reports a typed reason and does not enter kernel.
/// The source borrow ends when this call returns; entries and reservation
/// declarations are retained in loader storage, never a pointer to file bytes.
[[nodiscard]] KernelElfError LoadKernelImage(BootServices& services, Executable::Elf::Elf64ByteView source) noexcept;
/// Allocates the note's reservations and, where the kernel asks for one (KernelQuery:
/// MetadataArenaPages for a managed span of spanGiB GiB; AMD64 only), the metadata arena: 2
/// MiB-aligned LoaderData below every fixed user range.
[[nodiscard]] bool ReserveLoadedKernelMemory(BootServices& services, Foundation::UInt64 spanGiB) noexcept;
[[nodiscard]] const BootImagePlacement& GetPlacedKernelImage() noexcept;
[[nodiscard]] BootReservation GetKernelBootReservation(Foundation::UInt32 kind) noexcept;
/// The metadata arena's receipt; zero where the kernel asked for none.
[[nodiscard]] BootReservation GetKernelMetadataArena() noexcept;
[[nodiscard]] bool ReleaseKernelBootReservations(BootServices& services) noexcept;
[[nodiscard]] Foundation::UInt64 ReadKernelQuery(KernelQuery query, Foundation::UInt32 index = 0U) noexcept;
[[nodiscard]] const char* ReadKernelDiagnosticString(KernelQuery query) noexcept;
[[nodiscard]] bool PrepareLoadedKernel(BootInfo& info) noexcept;
[[noreturn]] void EnterLoadedKernel(const BootInfo& info) noexcept;
} // Namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_KERNEL_LOADER_HPP_INCLUDED
