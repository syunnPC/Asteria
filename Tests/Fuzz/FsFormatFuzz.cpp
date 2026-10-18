// SPDX-License-Identifier: Apache-2.0
// Raw sector/block inputs fit the common fuzz driver's 4096-byte capacity.
// Superblocks are tried in both slots; table/directory seeds use blocks 16/17.
// Check rejection atomicity, canonical round trips and location/generation
// binding. A second pass repairs CRCs to reach semantic validation as well.
#include "User/Filesystem/FsFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
namespace Fs = Asteria::User::Filesystem;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
UInt8 g_seed[Fs::FsBlockBytes]{};
UInt8 g_encoded[Fs::FsBlockBytes]{};
UInt8 g_repaired[Fs::FsBlockBytes]{};
Fs::FsObject g_objects[Fs::FsObjectCapacity]{};
Fs::FsObject g_beforeObjects[Fs::FsObjectCapacity]{};
Fs::FsDirectoryEntry g_directory[Fs::FsDirectoryCapacity]{};
Fs::FsDirectoryEntry g_beforeDirectory[Fs::FsDirectoryCapacity]{};

void Require(const bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}

void CheckSuperblock(const UInt8* bytes, const Size size, const UInt64 sector) noexcept
{
    Fs::FsSuperblock output{};
    output.VolumeGeneration = 0xCAFEU;
    Fs::FsSuperblock before = output;
    if (Fs::FsFormat::DecodeSuperblock(bytes, size, sector, output))
    {
        Require(Fs::FsFormat::EncodeSuperblock(output, sector, g_encoded, Fs::FsSectorBytes));
        Require(std::memcmp(bytes, g_encoded, Fs::FsSectorBytes) == 0);
        const UInt64 other = (sector == Fs::FsSuperblockA) ? Fs::FsSuperblockB : Fs::FsSuperblockA;
        before = output;
        Require(!Fs::FsFormat::DecodeSuperblock(bytes, size, other, output));
        Require(std::memcmp(&output, &before, sizeof(output)) == 0);
    }
    else
    {
        Require(std::memcmp(&output, &before, sizeof(output)) == 0);
    }
}

void CheckMetadata(const UInt8* bytes, const Size size) noexcept
{
    const UInt64 generation = (size >= Fs::FsMetadataLayout::HeaderBytes)
                                  ? Fs::FsFormat::ReadLittle(bytes + Fs::FsMetadataLayout::VolumeGeneration, 8U)
                                  : 0U;
    std::memcpy(g_beforeObjects, g_objects, sizeof(g_objects));
    if (Fs::FsFormat::DecodeObjectTable(bytes, size, 16U, generation, g_objects))
    {
        Require(Fs::FsFormat::EncodeObjectTable(g_objects, 16U, generation, g_encoded, Fs::FsBlockBytes));
        Require(std::memcmp(bytes, g_encoded, Fs::FsBlockBytes) == 0);
        std::memcpy(g_beforeObjects, g_objects, sizeof(g_objects));
        Require(!Fs::FsFormat::DecodeObjectTable(bytes, size, 16U, generation ^ 1U, g_objects));
        Require(!Fs::FsFormat::DecodeObjectTable(bytes, size, 17U, generation, g_objects));
        Require(std::memcmp(g_objects, g_beforeObjects, sizeof(g_objects)) == 0);
    }
    else
    {
        Require(std::memcmp(g_objects, g_beforeObjects, sizeof(g_objects)) == 0);
    }
    std::memcpy(g_beforeDirectory, g_directory, sizeof(g_directory));
    if (Fs::FsFormat::DecodeDirectory(bytes, size, 17U, generation, g_directory))
    {
        Require(Fs::FsFormat::EncodeDirectory(g_directory, 17U, generation, g_encoded, Fs::FsBlockBytes));
        Require(std::memcmp(bytes, g_encoded, Fs::FsBlockBytes) == 0);
        std::memcpy(g_beforeDirectory, g_directory, sizeof(g_directory));
        Require(!Fs::FsFormat::DecodeDirectory(bytes, size, 17U, generation ^ 1U, g_directory));
        Require(!Fs::FsFormat::DecodeDirectory(bytes, size, 16U, generation, g_directory));
        Require(std::memcmp(g_directory, g_beforeDirectory, sizeof(g_directory)) == 0);
    }
    else
    {
        Require(std::memcmp(g_directory, g_beforeDirectory, sizeof(g_directory)) == 0);
    }
}

void EmitVariants(void (*emit)(const std::uint8_t*, std::size_t), const Size bytes) noexcept
{
    emit(g_seed, bytes);
    g_seed[0U] ^= 1U;
    emit(g_seed, bytes);
    g_seed[0U] ^= 1U;
    g_seed[24U] ^= 1U;
    emit(g_seed, bytes);
    g_seed[24U] ^= 1U;
    emit(g_seed, bytes - 1U);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    CheckSuperblock(data, size, Fs::FsSuperblockA);
    CheckSuperblock(data, size, Fs::FsSuperblockB);
    CheckMetadata(data, size);
    if (size == Fs::FsSectorBytes)
    {
        constexpr UInt64 sectors[2U]{Fs::FsSuperblockA, Fs::FsSuperblockB};
        for (const UInt64 sector : sectors)
        {
            std::memcpy(g_repaired, data, size);
            const UInt64 generation = Fs::FsFormat::ReadLittle(data + Fs::FsSuperblockLayout::VolumeGeneration, 8U);
            Fs::FsFormat::WriteLittle(
                g_repaired + Fs::FsSuperblockLayout::Crc, 4U,
                Fs::FsFormat::Checksum(g_repaired, size, Fs::FsSuperblockLayout::Crc, sector, generation));
            CheckSuperblock(g_repaired, size, sector);
        }
    }
    else if (size == Fs::FsBlockBytes)
    {
        const UInt64 kind = Fs::FsFormat::ReadLittle(data + Fs::FsMetadataLayout::Kind, 4U);
        const UInt64 block = (kind == 1U) ? 16U : 17U;
        const UInt64 generation = Fs::FsFormat::ReadLittle(data + Fs::FsMetadataLayout::VolumeGeneration, 8U);
        std::memcpy(g_repaired, data, size);
        Fs::FsFormat::WriteLittle(
            g_repaired + Fs::FsMetadataLayout::Crc, 4U,
            Fs::FsFormat::Checksum(g_repaired, size, Fs::FsMetadataLayout::Crc, block, generation));
        CheckMetadata(g_repaired, size);
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    Fs::FsSuperblock superblock{};
    superblock.VolumeGeneration = 1U;
    superblock.TableBlock = 16U;
    superblock.NextObjectId = 1U;
    Require(Fs::FsFormat::EncodeSuperblock(superblock, Fs::FsSuperblockA, g_seed, Fs::FsSectorBytes));
    EmitVariants(emit, Fs::FsSectorBytes);
    Require(Fs::FsFormat::EncodeSuperblock(superblock, Fs::FsSuperblockB, g_seed, Fs::FsSectorBytes));
    EmitVariants(emit, Fs::FsSectorBytes);
    g_objects[0U].Kind = Fs::FsObjectKind::Directory;
    g_objects[0U].BirthGeneration = 1U;
    g_objects[0U].SizeBytes = Fs::FsBlockBytes;
    g_objects[0U].Direct[0U] = 17U;
    Require(Fs::FsFormat::EncodeObjectTable(g_objects, 16U, 1U, g_seed, Fs::FsBlockBytes));
    EmitVariants(emit, Fs::FsBlockBytes);
    g_directory[0U].NameLength = 1U;
    g_directory[0U].Name[0U] = 'a';
    g_directory[0U].ObjectId = 1U;
    Require(Fs::FsFormat::EncodeDirectory(g_directory, 17U, 1U, g_seed, Fs::FsBlockBytes));
    EmitVariants(emit, Fs::FsBlockBytes);
}
