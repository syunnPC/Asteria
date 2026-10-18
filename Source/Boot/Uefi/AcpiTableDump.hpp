// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_ACPI_TABLE_DUMP_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_ACPI_TABLE_DUMP_HPP_INCLUDED

#include "Boot/Acpi/AcpiTableParser.hpp"
#include "Boot/Uefi/UefiTypes.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::Boot::Uefi
{
// EFI protocol ABI prefixes. Only firmware owns the pointed-to objects.
struct File;
using FileOpen = Status(ASTERIA_UEFI_API*)(File*, File**, Foundation::UInt16*, Foundation::UInt64, Foundation::UInt64);
using FileClose = Status(ASTERIA_UEFI_API*)(File*);
using FileSetPosition = Status(ASTERIA_UEFI_API*)(File*, Foundation::UInt64);
using FileWrite = Status(ASTERIA_UEFI_API*)(File*, Foundation::Size*, const void*);
using FileRead = Status(ASTERIA_UEFI_API*)(File*, Foundation::Size*, void*);
using FileGetInfo = Status(ASTERIA_UEFI_API*)(File*, Guid*, Foundation::Size*, void*);
struct File
{
    Foundation::UInt64 Revision;
    FileOpen Open;
    FileClose Close;
    FileClose Delete;
    FileRead Read;
    FileWrite Write;
    void* GetPosition;
    FileSetPosition SetPosition;
    FileGetInfo GetInfo;
    void* SetInfo;
    FileClose Flush;
};
struct FileSystem;
using OpenVolume = Status(ASTERIA_UEFI_API*)(FileSystem*, File**);
struct FileSystem
{
    Foundation::UInt64 Revision;
    OpenVolume Open;
};
static_assert(__builtin_offsetof(File, Write) == 40U);
static_assert(__builtin_offsetof(File, Flush) == 80U);
static_assert(sizeof(FileSystem) == 16U);

constexpr Foundation::Size AcpiDumpWriteChunkBytes{65'536U};
constexpr Foundation::Size AcpiDumpNameCharacters{13U};
/// Produces an 8.3 name into AcpiDumpNameCharacters writable words. Signature
/// contains four legal FAT short-name characters; ordinal is zero for a unique
/// signature, else 1..2049 (the maximum root-plus-DSDT visit count).
[[nodiscard]] bool AcpiDumpName(const char* signature, Foundation::UInt32 ordinal, Foundation::UInt16* name) noexcept;
/// Replaces an existing file, returns an open writable file with Write/Flush/Close.
/// Caller must close it. Null on failure; no allocation by the adapter.
[[nodiscard]] File* OpenAcpiFile(File& directory, Foundation::UInt16* name) noexcept;
/// Firmware ABI adapter: Write borrows immutable input bytes. Deletes an
/// existing file before creation; writes at most 64 KiB per call, handles short
/// writes, flushes and closes. No allocation. Failure never changes boot policy.
[[nodiscard]] bool SaveAcpiFile(File& directory, Foundation::UInt16* name, const Foundation::UInt8* bytes,
                                Foundation::Size length) noexcept;
/// Appends an outcome to the already saved report and flushes/closes it.
[[nodiscard]] bool AppendAcpiFile(File& directory, Foundation::UInt16* name, const Foundation::UInt8* bytes,
                                  Foundation::Size length) noexcept;
/// Creates ACPI below EFI/ASTERIA, then dumps checksummed SDTs, including the
/// root and FADT-selected DSDTs. RSDP and FACS are excluded. BSP, pre-exit only.
void DumpAcpiTables(File& directory, const BootInfo& snapshot, Inventory::ReportText& text) noexcept;
} // namespace Asteria::Boot::Uefi

#endif // ASTERIA_BOOT_UEFI_ACPI_TABLE_DUMP_HPP_INCLUDED
