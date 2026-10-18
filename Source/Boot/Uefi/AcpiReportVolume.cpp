// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/AcpiReportVolume.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
[[nodiscard]] bool Name(const char* text, Foundation::UInt16* output) noexcept
{
    for (Foundation::Size i{0U}; i < AcpiDumpNameCharacters; ++i)
    {
        output[i] = static_cast<Foundation::UInt16>(text[i]);
        if (text[i] == '\0')
        {
            return true;
        }
    }
    return false;
}
}
FirmwareReportVolume::FirmwareReportVolume(File& directory) noexcept : m_directory{directory}
{
}
FilePresence FirmwareReportVolume::Exists(const char* text) noexcept
{
    Foundation::UInt16 name[AcpiDumpNameCharacters]{};
    if (m_directory.Open == nullptr || !Name(text, name))
    {
        return FilePresence::Failed;
    }
    File* file{nullptr};
    const Status status{m_directory.Open(&m_directory, &file, name, 1U, 0U)};
    if (status == NotFound)
    {
        return FilePresence::Missing;
    }
    if (status != Success || file == nullptr || file->Close == nullptr)
    {
        return FilePresence::Failed;
    }
    return file->Close(file) == Success ? FilePresence::Present : FilePresence::Failed;
}
bool FirmwareReportVolume::Start(const char* text) noexcept
{
    Foundation::UInt16 name[AcpiDumpNameCharacters]{};
    if (!Name(text, name))
    {
        return false;
    }
    m_file = OpenAcpiFile(m_directory, name);
    return m_file != nullptr;
}
bool FirmwareReportVolume::Append(const char* bytes, const Foundation::Size length) noexcept
{
    Foundation::Size offset{0U};
    while (offset < length)
    {
        const Foundation::Size remaining{length - offset};
        const Foundation::Size requested{remaining < AcpiDumpWriteChunkBytes ? remaining : AcpiDumpWriteChunkBytes};
        Foundation::Size written{requested};
        if (m_file == nullptr || m_file->Write(m_file, &written, bytes + offset) != Success || written == 0U ||
            written > requested)
        {
            return false;
        }
        offset += written;
    }
    return true;
}
bool FirmwareReportVolume::Finish() noexcept
{
    if (m_file == nullptr)
    {
        return false;
    }
    bool good{m_file->Flush(m_file) == Success};
    good = m_file->Close(m_file) == Success && good;
    m_file = nullptr;
    return good;
}
}
