// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_MANIFEST_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_MANIFEST_HPP_INCLUDED

#include "Boot/BootFiles.hpp"
#include "Boot/BootTrust.hpp"
#include "Boot/DisplayPolicy.hpp"

namespace Asteria::Boot
{
enum class BootManifestError : Foundation::UInt32
{
    None,
    TooLarge,
    TooManyModules,
    Syntax,
    Version,
    UnknownKey,
    Duplicate,
    MissingRequired,
    InvalidName,
    InvalidCrc,
    InvalidDisplay,
    InvalidTrust,
    TooManyTrust,
    TrustModule,
    InvalidLowMemory,
    InvalidWatchdog,
    InvalidSession,
    InvalidLogin,
};

/// Bounded ASCII parser, no allocation. Output is unchanged on failure. First key must be
/// version=1; kernel/init/user required, spinner, display, module, trust, lowmemory and watchdog
/// optional. Comments start at #; spaces, tabs and CRLF accepted. CRCs have exactly eight hex
/// digits. A trust line is `trust = [pci=SSSS:BB:DD.F] [id=VVVV:DDDD] driver=NAME module=NAME
/// sha256=HEX64`, its fields in that order, at least one selector, at most BootTrustMaximumEntries
/// lines, its module the drivers' (Abi::PagerDriverModuleName) and one a module line names; the
/// verdicts of a successful parse are Unchecked until the loader hashes the modules. A `lowmemory =
/// BLOCKS` line gives the low-memory watermark in free 2 MiB blocks below 4 GiB, decimal, 1 to
/// BootLowMemoryMaximumBlocks; without one lowMemory receives zero, the kernel's default. `watchdog
/// = SECONDS` (1–60) or `watchdog = off` selects both watchdogs; without one watchdogSeconds
/// receives WatchdogDefaultSeconds (Watchdog.md). Bytes and all outputs are disjoint borrows for
/// the duration of the call. Kernel is loader-only provenance; it is not part of the retained user
/// region.
[[nodiscard]] BootManifestError
ParseBootManifest(const Foundation::UInt8* bytes, Foundation::Size size, BootFileRegion& output, BootFileRecord& kernel,
                  DisplayPolicy* display = nullptr, BootTrustPolicy* trust = nullptr,
                  Foundation::UInt64* lowMemory = nullptr, Foundation::UInt64* watchdogSeconds = nullptr,
                  Foundation::UInt64* sessionMode = nullptr, Foundation::UInt64* loginEnabled = nullptr) noexcept;
[[nodiscard]] const char* BootManifestReason(BootManifestError error) noexcept;
} // Namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_MANIFEST_HPP_INCLUDED
