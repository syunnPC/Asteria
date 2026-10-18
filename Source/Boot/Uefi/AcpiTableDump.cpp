// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/AcpiTableDump.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Foundation::UInt64 Create{0x8000'0000'0000'0003ULL};
constexpr Foundation::Size MaximumDumpOrdinal{2U * Acpi::MaximumAcpiRootTableEntries + 1U};
constexpr Foundation::Size MaximumDumpSignatures{Acpi::MaximumAcpiRootTableEntries + 2U};
struct SignatureCount
{
    char Signature[4U];
    Foundation::UInt16 Count;
    Foundation::UInt16 Written;
};
// BSP-only metadata; no table bytes are copied into the image.
constinit SignatureCount g_signatures[MaximumDumpSignatures]{};
class DumpVisitor final : public Acpi::IAcpiTableVisitor
{
public:
    File* Directory;
    Inventory::ReportText* Text;
    Foundation::Size Signatures;
    Foundation::UInt32 Saved;
    Foundation::UInt32 Failed;
    bool Counting;
    void Visit(const Acpi::AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept override;
};

[[nodiscard]] bool FileCharacter(const char value) noexcept
{
    if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'))
    {
        return true;
    }
    constexpr char allowed[]{"_!#$%&'()-@^`{}~"};
    for (const char character : allowed)
    {
        if (character != '\0' && character == value)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool SameSignature(const char* left, const char* right) noexcept
{
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

void CountTable(void* context, const Acpi::AcpiTableRecord& record, const Foundation::UInt8*) noexcept
{
    if (record.ChecksumValid == 0U)
    {
        return;
    }
    auto& dump = *static_cast<DumpVisitor*>(context);
    for (Foundation::Size index{0U}; index < dump.Signatures; ++index)
    {
        if (SameSignature(g_signatures[index].Signature, record.Signature))
        {
            ++g_signatures[index].Count;
            return;
        }
    }
    if (dump.Signatures < MaximumDumpSignatures)
    {
        auto& entry = g_signatures[dump.Signatures++];
        entry = {};
        for (Foundation::Size index{0U}; index < 4U; ++index)
        {
            entry.Signature[index] = record.Signature[index];
        }
        entry.Count = 1U;
    }
}

void WriteTable(void* context, const Acpi::AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept
{
    if (record.ChecksumValid == 0U)
    {
        return;
    }
    auto& dump = *static_cast<DumpVisitor*>(context);
    Foundation::UInt16 name[AcpiDumpNameCharacters]{};
    bool saved{false};
    for (Foundation::Size index{0U}; index < dump.Signatures; ++index)
    {
        auto& entry = g_signatures[index];
        if (SameSignature(entry.Signature, record.Signature))
        {
            ++entry.Written;
            saved = AcpiDumpName(record.Signature, entry.Count > 1U ? entry.Written : 0U, name) &&
                    SaveAcpiFile(*dump.Directory, name, bytes, record.Length);
            break;
        }
    }
    if (saved)
    {
        ++dump.Saved;
    }
    else
    {
        ++dump.Failed;
        if (dump.Failed <= 16U)
        {
            dump.Text->Text("ACPI dump ");
            for (const char value : record.Signature)
            {
                dump.Text->Character(value >= 32 && value <= 126 ? value : '?');
            }
            dump.Text->Text(" FAILED\r\n");
        }
    }
}
void DumpVisitor::Visit(const Acpi::AcpiTableRecord& record, const Foundation::UInt8* bytes) noexcept
{
    if (Counting)
    {
        CountTable(this, record, bytes);
    }
    else
    {
        WriteTable(this, record, bytes);
    }
}

[[nodiscard]] bool WriteChunks(File* file, const Foundation::UInt8* bytes, const Foundation::Size length) noexcept
{
    bool saved = file->Write != nullptr && file->Flush != nullptr && file->Close != nullptr;
    Foundation::Size offset{0U};
    while (saved && offset < length)
    {
        const Foundation::Size remaining = length - offset;
        const Foundation::Size requested = remaining < AcpiDumpWriteChunkBytes ? remaining : AcpiDumpWriteChunkBytes;
        Foundation::Size written{requested};
        saved = file->Write(file, &written, bytes + offset) == Success && written != 0U && written <= requested;
        if (saved)
        {
            offset += written;
        }
    }
    if (file->Flush != nullptr)
    {
        saved = file->Flush(file) == Success && saved;
    }
    if (file->Close != nullptr)
    {
        saved = file->Close(file) == Success && saved;
    }
    return saved;
}
} // namespace

bool AcpiDumpName(const char* signature, const Foundation::UInt32 ordinal, Foundation::UInt16* name) noexcept
{
    if (signature == nullptr || name == nullptr || ordinal > MaximumDumpOrdinal)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        const char value = signature[index];
        if (!FileCharacter(value))
        {
            name[0U] = 0U;
            return false;
        }
        name[index] = static_cast<Foundation::UInt16>(value);
    }
    Foundation::Size position{4U};
    if (ordinal != 0U)
    {
        Foundation::UInt16 digits[4U]{};
        Foundation::UInt32 value{ordinal};
        for (Foundation::Size index{4U}; index != 0U; --index)
        {
            digits[index - 1U] = static_cast<Foundation::UInt16>('0' + value % 10U);
            value /= 10U;
        }
        Foundation::Size first{0U};
        while (first < 3U && digits[first] == '0')
        {
            ++first;
        }
        for (Foundation::Size index{first}; index < 4U; ++index)
        {
            if (position >= 8U)
            {
                return false;
            }
            name[position++] = digits[index];
        }
    }
    if (position > 8U)
    {
        return false;
    }
    name[position++] = '.';
    name[position++] = 'A';
    name[position++] = 'M';
    name[position++] = 'L';
    name[position] = 0U;
    return true;
}

File* OpenAcpiFile(File& directory, Foundation::UInt16* name) noexcept
{
    if (directory.Open == nullptr)
    {
        return nullptr;
    }
    File* file{nullptr};
    const Status existing = directory.Open(&directory, &file, name, 3U, 0U);
    if (existing == Success && file != nullptr)
    {
        if (file->Delete == nullptr || file->Delete(file) != Success)
        {
            return nullptr;
        }
    }
    else if (existing != NotFound)
    {
        return nullptr;
    }
    file = nullptr;
    if (directory.Open(&directory, &file, name, Create, 0U) != Success || file == nullptr)
    {
        return nullptr;
    }
    if (file->Write == nullptr || file->Flush == nullptr || file->Close == nullptr)
    {
        if (file->Close != nullptr)
        {
            static_cast<void>(file->Close(file));
        }
        return nullptr;
    }
    return file;
}

bool SaveAcpiFile(File& directory, Foundation::UInt16* name, const Foundation::UInt8* bytes,
                  const Foundation::Size length) noexcept
{
    if (bytes == nullptr || length == 0U || length > 1'048'576U)
    {
        return false;
    }
    File* const file{OpenAcpiFile(directory, name)};
    return file != nullptr && WriteChunks(file, bytes, length);
}

bool AppendAcpiFile(File& directory, Foundation::UInt16* name, const Foundation::UInt8* bytes,
                    const Foundation::Size length) noexcept
{
    if (directory.Open == nullptr || bytes == nullptr || length == 0U || length > 1'048'576U)
    {
        return false;
    }
    File* file{nullptr};
    if (directory.Open(&directory, &file, name, 3U, 0U) != Success || file == nullptr)
    {
        return false;
    }
    if (file->SetPosition == nullptr || file->SetPosition(file, ~Foundation::UInt64{0U}) != Success)
    {
        if (file->Close != nullptr)
        {
            static_cast<void>(file->Close(file));
        }
        return false;
    }
    return WriteChunks(file, bytes, length);
}

void DumpAcpiTables(File& directory, const BootInfo& snapshot, Inventory::ReportText& text) noexcept
{
    if (snapshot.Acpi.RootKind == Acpi::AcpiRootTableKind::None)
    {
        text.Text("ACPI dump FAILED: snapshot unavailable\r\n");
        return;
    }
    Foundation::UInt16 name[]{'A', 'C', 'P', 'I', 0U};
    File* acpi{nullptr};
    if (directory.Open == nullptr || directory.Open(&directory, &acpi, name, Create, 0x10U) != Success ||
        acpi == nullptr)
    {
        text.Text("ACPI dump FAILED: directory unavailable\r\n");
        return;
    }
    DumpVisitor dump{};
    dump.Directory = acpi;
    dump.Text = &text;
    dump.Counting = true;
    Acpi::AcpiTableParser::VisitTables(snapshot, snapshot.Acpi, dump);
    dump.Counting = false;
    Acpi::AcpiTableParser::VisitTables(snapshot, snapshot.Acpi, dump);
    text.Text("ACPI dump saved=");
    text.Decimal(dump.Saved);
    text.Text(" failed=");
    text.Decimal(dump.Failed);
    text.Text("\r\n");
    if (acpi->Close == nullptr || acpi->Close(acpi) != Success)
    {
        text.Text("ACPI dump FAILED: directory close\r\n");
    }
}
} // namespace Asteria::Boot::Uefi
