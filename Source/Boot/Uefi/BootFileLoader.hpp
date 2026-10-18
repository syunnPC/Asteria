// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_BOOT_FILE_LOADER_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_BOOT_FILE_LOADER_HPP_INCLUDED

#include "Boot/BootFiles.hpp"
#include "Boot/BootTrust.hpp"
#include "Boot/DisplayPolicy.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::Boot::Uefi
{
/// Loader-owned temporary receipt. Pages contain exactly File.SizeBytes ELF
/// bytes plus zero page padding, are separate from the retained user region,
/// and must be released after placement, before the final-map transaction.
struct KernelFileReceipt
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 PageCount;
    BootFileRecord File;
};

/// BSP only, pre-exit, one attempt. Opens the loaded image's volume, reads bounded whole files and
/// verifies CRC-32C in a retained LoaderData user region and a separate temporary kernel allocation
/// (at most 16 MiB). Hashes every module a trust entry names with SHA-256 and records each entry's
/// verdict (Verified or DigestMismatch): a mismatch never fails the boot, it leaves the entry
/// inert. Retains allocated pages even on failure; closes every opened file. Returns a diagnostic
/// reason (nullptr on success); outputs change only on success. lowMemory receives BOOT.CFG's
/// `lowmemory` blocks, zero without the key.
[[nodiscard]] const char* LoadBootFiles(Handle image, BootServices& services, BootFileRegion& output,
                                        KernelFileReceipt& kernel, DisplayPolicy* display = nullptr,
                                        BootTrustPolicy* trust = nullptr, Foundation::UInt64* lowMemory = nullptr,
                                        Foundation::UInt64* watchdogSeconds = nullptr,
                                        Foundation::UInt64* sessionMode = nullptr,
                                        Foundation::UInt64* loginEnabled = nullptr) noexcept;
/// Appends one stage record; moduleName is null for an initial image.
void FormatBootFile(Inventory::ReportText& text, const BootFileRecord& file, const char* moduleName) noexcept;
/// Appends a size and CRC provenance report line (not a stage event) for every
/// present file, without allocation.
void FormatBootFiles(Inventory::ReportText& text, const BootFileRegion& region) noexcept;
/// Appends "ASTERIA:BOOT-TRUST:<n> pci=... id=... driver=... module=... sha256=<hex> <VERIFIED|DIGEST-MISMATCH>"
/// for the one-based entry number, without a line end; modules names the entry's module.
void FormatBootTrust(Inventory::ReportText& text, const BootTrustEntry& entry, Foundation::Size number,
                     const BootFileRegion& modules) noexcept;
} // Namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_BOOT_FILE_LOADER_HPP_INCLUDED
