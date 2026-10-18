// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_ACPI_REPORT_VOLUME_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_ACPI_REPORT_VOLUME_HPP_INCLUDED

#include "Boot/Uefi/AcpiTableDump.hpp"

namespace Asteria::Boot::Uefi
{
enum class FilePresence : Foundation::UInt8
{
    Missing,
    Present,
    Failed,
};
/// Serialized report destination rooted at EFI/ASTERIA on the loaded volume.
/// Names are uppercase 8.3, optionally under ACPI/. One open file at a time.
/// Start replaces the file; Append consumes borrowed bytes synchronously;
/// Finish flushes/closes even after an Append failure. No hidden allocation.
class IAcpiReportVolume
{
public:
    [[nodiscard]] virtual FilePresence Exists(const char* name) noexcept = 0;
    [[nodiscard]] virtual bool Start(const char* name) noexcept = 0;
    [[nodiscard]] virtual bool Append(const char* bytes, Foundation::Size length) noexcept = 0;
    [[nodiscard]] virtual bool Finish() noexcept = 0;

protected:
    ~IAcpiReportVolume() noexcept = default;
};
/// EFI file protocol adapter. Borrows the already open EFI/ASTERIA directory.
class FirmwareReportVolume final : public IAcpiReportVolume
{
public:
    explicit FirmwareReportVolume(File& directory) noexcept;
    [[nodiscard]] FilePresence Exists(const char* name) noexcept override;
    [[nodiscard]] bool Start(const char* name) noexcept override;
    [[nodiscard]] bool Append(const char* bytes, Foundation::Size length) noexcept override;
    [[nodiscard]] bool Finish() noexcept override;

private:
    File& m_directory;
    File* m_file{nullptr};
};
}
#endif
