// SPDX-License-Identifier: Apache-2.0
#include "User/Filesystem/FsFormat.hpp"

#include <cstring>

#include "Executive/ObjectDirectory/ObjectDirectoryTypes.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test::Filesystem
{
namespace
{
namespace Fs = User::Filesystem;
using TestSupport::TestContext;
using Foundation::Size;
using Foundation::UInt8;
UInt8 g_bytes[Fs::FsBlockBytes]{};
UInt8 g_copy[Fs::FsBlockBytes]{};
Fs::FsObject g_objects[Fs::FsObjectCapacity]{};
Fs::FsObject g_decodedObjects[Fs::FsObjectCapacity]{};
Fs::FsDirectoryEntry g_directory[Fs::FsDirectoryCapacity]{};
Fs::FsDirectoryEntry g_decodedDirectory[Fs::FsDirectoryCapacity]{};

void TestSuperblock(TestContext& context) noexcept
{
    Fs::FsSuperblock value{};
    value.VolumeGeneration = 42U;
    value.TableBlock = 23U;
    value.TableCrc = 0x87654321U;
    value.NextObjectId = 2U;
    value.Committed[0U] = Fs::FsCommitRecord{.Sender = 0x1122334455667788U, .Transaction = 5U};
    value.CommitGenerations[0U] = 40U;
    value.Bitmap[3U] = 0x56U;
    Fs::FsSuperblock decoded{};
    ASTERIA_EXPECT(context, "one-sector superblock field codec is canonical little endian",
                   Fs::FsFormat::EncodeSuperblock(value, Fs::FsSuperblockB, g_bytes, Fs::FsSectorBytes),
                   g_bytes[Fs::FsSuperblockLayout::VolumeGeneration] == 42U, g_bytes[48U] == 0x88U,
                   g_bytes[55U] == 0x11U,
                   Fs::FsFormat::DecodeSuperblock(g_bytes, Fs::FsSectorBytes, Fs::FsSuperblockB, decoded),
                   decoded.VolumeGeneration == 42U, decoded.TableBlock == 23U, decoded.TableCrc == 0x87654321U,
                   decoded.Committed[0U].Sender == value.Committed[0U].Sender, decoded.CommitGenerations[0U] == 40U,
                   decoded.Bitmap[3U] == 0x56U,
                   Fs::FsFormat::EncodeSuperblock(decoded, Fs::FsSuperblockB, g_copy, Fs::FsSectorBytes),
                   std::memcmp(g_bytes, g_copy, Fs::FsSectorBytes) == 0);
    const Fs::FsSuperblock before = decoded;
    ASTERIA_EXPECT(context, "superblock refuses bad sizes, nulls, and a misdirected sector atomically",
                   !Fs::FsFormat::DecodeSuperblock(g_bytes, 511U, Fs::FsSuperblockB, decoded),
                   !Fs::FsFormat::DecodeSuperblock(nullptr, 512U, Fs::FsSuperblockB, decoded),
                   !Fs::FsFormat::DecodeSuperblock(g_bytes, 512U, Fs::FsSuperblockA, decoded),
                   std::memcmp(&before, &decoded, sizeof(decoded)) == 0);
    const Size mutations[8U]{0U, 8U, 12U, 16U, 24U, 36U, 208U, 508U};
    for (const Size offset : mutations)
    {
        g_bytes[offset] ^= 1U;
        ASTERIA_EXPECT(context, "bad magic, version, geometry, generation, reserved fields and CRC are refused",
                       !Fs::FsFormat::DecodeSuperblock(g_bytes, 512U, Fs::FsSuperblockB, decoded),
                       std::memcmp(&before, &decoded, sizeof(decoded)) == 0);
        g_bytes[offset] ^= 1U;
    }
    g_bytes[36U] = 1U;
    Fs::FsFormat::WriteLittle(g_bytes + 508U, 4U, Fs::FsFormat::Checksum(g_bytes, 512U, 508U, 16U, 42U));
    ASTERIA_EXPECT(context, "unsupported fields are refused even with a valid checksum",
                   !Fs::FsFormat::DecodeSuperblock(g_bytes, 512U, 16U, decoded));
    value.VolumeGeneration = 0U;
    std::memcpy(g_bytes, g_copy, 512U);
    ASTERIA_EXPECT(context, "invalid encode leaves the destination unchanged",
                   !Fs::FsFormat::EncodeSuperblock(value, 16U, g_bytes, 512U), std::memcmp(g_bytes, g_copy, 512U) == 0);
}

void TestMetadata(TestContext& context) noexcept
{
    g_objects[0U].BirthGeneration = 1U;
    g_objects[0U].SizeBytes = Fs::FsBlockBytes;
    g_objects[0U].Kind = Fs::FsObjectKind::Directory;
    g_objects[0U].Direct[0U] = 17U;
    g_objects[1U].BirthGeneration = 7U;
    g_objects[1U].Kind = Fs::FsObjectKind::File;
    g_objects[1U].SizeBytes = Fs::FsDirectCapacity * Fs::FsBlockBytes;
    g_objects[1U].Direct[5U] = 511U;
    ASTERIA_EXPECT(context, "object table round trips every field and all six direct slots",
                   Fs::FsFormat::EncodeObjectTable(g_objects, 16U, 7U, g_bytes, sizeof(g_bytes)),
                   Fs::FsFormat::DecodeObjectTable(g_bytes, sizeof(g_bytes), 16U, 7U, g_decodedObjects),
                   g_decodedObjects[1U].BirthGeneration == 7U, g_decodedObjects[1U].Direct[5U] == 511U,
                   Fs::FsFormat::EncodeObjectTable(g_decodedObjects, 16U, 7U, g_copy, sizeof(g_copy)),
                   std::memcmp(g_bytes, g_copy, sizeof(g_bytes)) == 0);
    ASTERIA_EXPECT(context, "metadata refuses wrong block, generation, kind, size and null",
                   !Fs::FsFormat::DecodeObjectTable(g_bytes, sizeof(g_bytes), 17U, 7U, g_decodedObjects),
                   !Fs::FsFormat::DecodeObjectTable(g_bytes, sizeof(g_bytes), 16U, 8U, g_decodedObjects),
                   !Fs::FsFormat::DecodeObjectTable(g_bytes, 4095U, 16U, 7U, g_decodedObjects),
                   !Fs::FsFormat::DecodeObjectTable(nullptr, 4096U, 16U, 7U, g_decodedObjects),
                   !Fs::FsFormat::DecodeDirectory(g_bytes, 4096U, 16U, 7U, g_decodedDirectory));
    const Size mutations[5U]{0U, 8U, 12U, 20U, 24U};
    for (const Size offset : mutations)
    {
        g_bytes[offset] ^= 1U;
        ASTERIA_EXPECT(context, "damaged metadata header is rejected",
                       !Fs::FsFormat::DecodeObjectTable(g_bytes, 4096U, 16U, 7U, g_decodedObjects));
        g_bytes[offset] ^= 1U;
    }
    Fs::FsFormat::WriteLittle(g_bytes + 32U + 32U + 16U, 4U, 99U);
    Fs::FsFormat::WriteLittle(g_bytes + Fs::FsMetadataLayout::Crc, 4U,
                              Fs::FsFormat::Checksum(g_bytes, 4096U, Fs::FsMetadataLayout::Crc, 16U, 7U));
    ASTERIA_EXPECT(context, "invalid object kind with valid CRC leaves the entire output untouched",
                   !Fs::FsFormat::DecodeObjectTable(g_bytes, 4096U, 16U, 7U, g_decodedObjects),
                   g_decodedObjects[1U].Kind == Fs::FsObjectKind::File);

    g_directory[0U].NameLength = 64U;
    g_directory[0U].ObjectId = 1U;
    for (Size index{0U}; index < 64U; ++index)
    {
        g_directory[0U].Name[index] = 'a';
    }
    ASTERIA_EXPECT(context, "directory supports full DirectoryName length and canonical round trip",
                   Fs::FsFormat::EncodeDirectory(g_directory, 18U, 7U, g_bytes, 4096U),
                   Fs::FsFormat::DecodeDirectory(g_bytes, 4096U, 18U, 7U, g_decodedDirectory),
                   g_decodedDirectory[0U].NameLength == 64U, g_decodedDirectory[0U].ObjectId == 1U,
                   Fs::FsFormat::EncodeDirectory(g_decodedDirectory, 18U, 7U, g_copy, 4096U),
                   std::memcmp(g_bytes, g_copy, 4096U) == 0);
    g_bytes[32U + 4U] = '-';
    Fs::FsFormat::WriteLittle(g_bytes + Fs::FsMetadataLayout::Crc, 4U,
                              Fs::FsFormat::Checksum(g_bytes, 4096U, Fs::FsMetadataLayout::Crc, 18U, 7U));
    ASTERIA_EXPECT(context, "invalid directory name with valid checksum is refused atomically",
                   !Fs::FsFormat::DecodeDirectory(g_bytes, 4096U, 18U, 7U, g_decodedDirectory),
                   g_decodedDirectory[0U].Name[0U] == 'a');
}

void TestNames(TestContext& context) noexcept
{
    UInt8 name[65U]{};
    for (Size size{0U}; size <= 65U; ++size)
    {
        for (Size index{0U}; index < size; ++index)
        {
            name[index] = 'a';
        }
        for (Size position{0U}; position < size; ++position)
        {
            for (Foundation::UInt32 byte{0U}; byte < 256U; ++byte)
            {
                name[position] = static_cast<UInt8>(byte);
                const auto expected = Executive::ObjectDirectory::DirectoryName::Create(
                    Executive::ObjectDirectory::DirectoryNameView{.Data = name, .SizeBytes = size});
                ASTERIA_EXPECT(context, "filesystem name rules equal DirectoryName for all bytes and boundaries",
                               Fs::FsFormat::ValidName(name, size) == expected.HasValue());
            }
            name[position] = 'a';
        }
    }
    ASTERIA_EXPECT(context, "empty and null names are invalid", !Fs::FsFormat::ValidName(name, 0U),
                   !Fs::FsFormat::ValidName(nullptr, 1U));
}
} // namespace
} // namespace Asteria::Test::Filesystem

int main() noexcept
{
    Asteria::TestSupport::TestContext context{"FsFormat"};
    Asteria::Test::Filesystem::TestSuperblock(context);
    Asteria::Test::Filesystem::TestMetadata(context);
    Asteria::Test::Filesystem::TestNames(context);
    return context.Finish();
}
