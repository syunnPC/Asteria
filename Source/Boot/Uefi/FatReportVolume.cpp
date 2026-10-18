// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/FatReportVolume.hpp"

#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::Boot::Uefi
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
constexpr Guid BlockGuid{0x964E5B21U, 0x6459U, 0x11D2U, {0x8EU, 0x39U, 0U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
struct BlockMedia
{
    UInt32 MediaId;
    Boolean Removable;
    Boolean Present;
    Boolean LogicalPartition;
    Boolean ReadOnly;
    Boolean WriteCaching;
    UInt32 BlockSize;
    UInt32 IoAlign;
    UInt64 LastBlock;
};
struct BlockIo;
using BlockRead = Status(ASTERIA_UEFI_API*)(BlockIo*, UInt32, UInt64, Size, void*);
using BlockWrite = Status(ASTERIA_UEFI_API*)(BlockIo*, UInt32, UInt64, Size, const void*);
using BlockFlush = Status(ASTERIA_UEFI_API*)(BlockIo*);
struct BlockIo
{
    UInt64 Revision;
    BlockMedia* Media;
    void* Reset;
    BlockRead Read;
    BlockWrite Write;
    BlockFlush Flush;
};
static_assert(__builtin_offsetof(BlockMedia, LastBlock) == 24U);
static_assert(__builtin_offsetof(BlockIo, Read) == 24U);
constexpr Size PageBytes{4096U};
constexpr Size StoragePages{3U};
constexpr UInt32 EndCluster{0x0FFF'FFFFU};
constexpr UInt32 MaximumClusters{1'048'576U};
constexpr Size MaximumFileBytes{16U * 1024U * 1024U};
constexpr Size MaximumDirectoryClusters{4096U};

[[nodiscard]] UInt32 Read(const UInt8* bytes, const Size width) noexcept
{
    UInt32 value{0U};
    for (Size i{0U}; i < width; ++i)
    {
        value |= UInt32{bytes[i]} << (i * 8U);
    }
    return value;
}
void Write(UInt8* bytes, UInt32 value, const Size width) noexcept
{
    for (Size i{0U}; i < width; ++i)
    {
        bytes[i] = static_cast<UInt8>(value & 255U);
        value >>= 8U;
    }
}
struct Entry
{
    UInt64 Sector{0U};
    Size Offset{0U};
    UInt32 Cluster{0U};
    UInt32 Bytes{0U};
    UInt8 Attributes{0U};
    bool Found{false};
};

class Volume final : public IAcpiReportVolume
{
public:
    explicit Volume(BlockIo& block) noexcept : m_block{block}
    {
    }
    [[nodiscard]] bool Mount() noexcept;
    [[nodiscard]] FilePresence Exists(const char* name) noexcept override;
    [[nodiscard]] bool Start(const char* name) noexcept override;
    [[nodiscard]] bool Append(const char* bytes, Size length) noexcept override;
    [[nodiscard]] bool Finish() noexcept override;

private:
    [[nodiscard]] bool Sector(UInt64 sector, bool writing = false) noexcept;
    [[nodiscard]] bool Flush() noexcept;
    [[nodiscard]] bool GetFat(UInt32 cluster, UInt32& value) noexcept;
    [[nodiscard]] bool SetFat(UInt32 cluster, UInt32 value) noexcept;
    [[nodiscard]] bool Allocate(UInt32& cluster) noexcept;
    [[nodiscard]] bool Find(UInt32 directory, const char* name, Entry& entry) noexcept;
    [[nodiscard]] bool FindPath(const char* name, Entry& entry) noexcept;
    [[nodiscard]] bool Drain() noexcept;
    [[nodiscard]] bool InvalidateFsInfo() noexcept;
    [[nodiscard]] bool FreeChain(UInt32 cluster) noexcept;
    [[nodiscard]] bool Cluster(UInt32 value) const noexcept;
    [[nodiscard]] UInt64 ClusterSector(UInt32 cluster) const noexcept;
    void DecodeEntry(const UInt8* bytes, Entry& entry) const noexcept;
    BlockIo& m_block;
    UInt32 m_mediaId{0U};
    UInt32 m_reserved{0U};
    UInt32 m_fatSectors{0U};
    UInt32 m_dataStart{0U};
    UInt32 m_clusters{0U};
    UInt32 m_clusterSectors{0U};
    UInt32 m_fsInfo{0U};
    UInt32 m_backupBoot{0U};
    UInt32 m_directory{0U};
    UInt32 m_acpiDirectory{0U};
    UInt32 m_nextFree{2U};
    UInt32 m_first{0U};
    UInt32 m_last{0U};
    UInt32 m_chainSectors{0U};
    Size m_bytes{0U};
    Size m_buffered{0U};
    Entry m_entry{};
    UInt8 m_name[11U]{};
    bool m_open{false};
    bool m_broken{false};
    alignas(PageBytes) UInt8 m_sector[512U]{};
    UInt8 m_data[512U]{};
};
static_assert(sizeof(Volume) <= StoragePages * PageBytes);

bool Volume::Sector(const UInt64 sector, const bool writing) noexcept
{
    if (m_broken || sector > m_block.Media->LastBlock || m_block.Media->MediaId != m_mediaId)
    {
        m_broken = true;
        return false;
    }
    const Status status{writing ? m_block.Write(&m_block, m_mediaId, sector, sizeof(m_sector), m_sector)
                                : m_block.Read(&m_block, m_mediaId, sector, sizeof(m_sector), m_sector)};
    m_broken = status != Success;
    return !m_broken;
}
bool Volume::Flush() noexcept
{
    if (m_broken || m_block.Flush(&m_block) != Success)
    {
        m_broken = true;
        return false;
    }
    return true;
}
bool Volume::Cluster(const UInt32 value) const noexcept
{
    return value >= 2U && value - 2U < m_clusters;
}
UInt64 Volume::ClusterSector(const UInt32 cluster) const noexcept
{
    return UInt64{m_dataStart} + UInt64{cluster - 2U} * m_clusterSectors;
}
bool Volume::GetFat(const UInt32 cluster, UInt32& value) noexcept
{
    if (cluster > m_clusters + 1U || !Sector(UInt64{m_reserved} + cluster / 128U))
    {
        return false;
    }
    value = Read(m_sector + (cluster % 128U) * 4U, 4U) & EndCluster;
    if (!Sector(UInt64{m_reserved} + m_fatSectors + cluster / 128U) ||
        (Read(m_sector + (cluster % 128U) * 4U, 4U) & EndCluster) != value)
    {
        m_broken = true;
        return false;
    }
    return true;
}
bool Volume::SetFat(const UInt32 cluster, const UInt32 value) noexcept
{
    for (Size copy{0U}; copy < 2U; ++copy)
    {
        const UInt64 sector{UInt64{m_reserved} + copy * UInt64{m_fatSectors} + cluster / 128U};
        if (!Sector(sector))
        {
            return false;
        }
        UInt8* const cell{m_sector + (cluster % 128U) * 4U};
        Write(cell, (Read(cell, 4U) & 0xF000'0000U) | value, 4U);
        if (!Sector(sector, true))
        {
            return false;
        }
    }
    return true;
}
bool Volume::Allocate(UInt32& cluster) noexcept
{
    for (Size i{0U}; i < m_clusters; ++i)
    {
        const UInt32 candidate{m_nextFree};
        ++m_nextFree;
        if (m_nextFree > m_clusters + 1U)
        {
            m_nextFree = 2U;
        }
        UInt32 value{0U};
        if (!GetFat(candidate, value))
        {
            return false;
        }
        if (value == 0U)
        {
            if (!SetFat(candidate, EndCluster))
            {
                return false;
            }
            cluster = candidate;
            return true;
        }
    }
    m_broken = true;
    return false;
}
void Volume::DecodeEntry(const UInt8* bytes, Entry& entry) const noexcept
{
    entry.Cluster = (Read(bytes + 20U, 2U) << 16U) | Read(bytes + 26U, 2U);
    entry.Bytes = Read(bytes + 28U, 4U);
    entry.Attributes = bytes[11U];
    entry.Found = true;
}
bool Volume::Find(const UInt32 directory, const char* name, Entry& entry) noexcept
{
    UInt8 shortName[11U]{};
    for (UInt8& byte : shortName)
    {
        byte = ' ';
    }
    Size cursor{0U};
    for (Size i{0U}; name[i] != '\0'; ++i)
    {
        if (name[i] == '.' && cursor <= 8U)
        {
            cursor = 8U;
        }
        else if (cursor < sizeof(shortName) &&
                 ((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
        {
            shortName[cursor++] = static_cast<UInt8>(name[i]);
        }
        else
        {
            return false;
        }
    }
    bool longMatch{false};
    UInt8 longChecksum{0U};
    Entry free{};
    UInt32 cluster{directory};
    for (Size visited{0U}; visited < MaximumDirectoryClusters && Cluster(cluster); ++visited)
    {
        for (Size sector{0U}; sector < m_clusterSectors; ++sector)
        {
            const UInt64 address{ClusterSector(cluster) + sector};
            if (!Sector(address))
            {
                return false;
            }
            for (Size offset{0U}; offset < sizeof(m_sector); offset += 32U)
            {
                const UInt8* const bytes{m_sector + offset};
                if ((bytes[0U] == 0U || bytes[0U] == 0xE5U) && free.Sector == 0U)
                {
                    free.Sector = address;
                    free.Offset = offset;
                }
                if (bytes[0U] == 0U)
                {
                    // A deleted slot keeps the original end marker. Otherwise
                    // refuse a terminal slot or stale following entry instead
                    // of exposing bytes that were beyond the directory end.
                    if (free.Sector == address && free.Offset == offset &&
                        (offset + 32U == sizeof(m_sector) || m_sector[offset + 32U] != 0U))
                    {
                        return false;
                    }
                    entry = free;
                    return true;
                }
                if (bytes[11U] == 0x0FU)
                {
                    // A single VFAT slot carries up to 13 UTF-16 characters.
                    // Only a complete, checksum-bound ASCII name is accepted.
                    constexpr Size positions[]{1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
                    longMatch = bytes[0U] == 0x41U && bytes[12U] == 0U && Read(bytes + 26U, 2U) == 0U;
                    bool ended{false};
                    for (Size character{0U}; character < 13U && longMatch; ++character)
                    {
                        const UInt32 value = Read(bytes + positions[character], 2U);
                        if (ended)
                        {
                            longMatch = value == 0xFFFFU;
                        }
                        else if (value == 0U)
                        {
                            longMatch = name[character] == '\0';
                            ended = true;
                        }
                        else
                        {
                            longMatch = name[character] != '\0' && value == static_cast<UInt8>(name[character]);
                        }
                    }
                    longMatch = longMatch && ended;
                    longChecksum = bytes[13U];
                    continue;
                }
                UInt8 checksum{0U};
                for (Size i{0U}; i < 11U; ++i)
                {
                    checksum = static_cast<UInt8>(((checksum & 1U) << 7U) + (checksum >> 1U) + bytes[i]);
                }
                const bool matchedLong = longMatch && checksum == longChecksum;
                longMatch = false;
                bool same{bytes[0U] != 0xE5U && (bytes[11U] & 8U) == 0U};
                for (Size i{0U}; same && i < 11U; ++i)
                {
                    same = bytes[i] == shortName[i];
                }
                if (same || (matchedLong && bytes[0U] != 0xE5U && (bytes[11U] & 8U) == 0U))
                {
                    entry = {address, offset};
                    DecodeEntry(bytes, entry);
                    return true;
                }
            }
        }
        UInt32 next{0U};
        if (!GetFat(cluster, next))
        {
            return false;
        }
        if (next >= 0x0FFF'FFF8U)
        {
            entry = free;
            return free.Sector != 0U;
        }
        cluster = next;
    }
    return false;
}
bool Volume::FindPath(const char* name, Entry& entry) noexcept
{
    UInt32 directory{m_directory};
    if (name[0U] == 'A' && name[1U] == 'C' && name[2U] == 'P' && name[3U] == 'I' && name[4U] == '/')
    {
        directory = m_acpiDirectory;
        name += 5U;
    }
    return Find(directory, name, entry);
}
bool Volume::Mount() noexcept
{
    const BlockMedia* const media{m_block.Media};
    if (media == nullptr || media->Present == 0U || media->ReadOnly != 0U || media->LogicalPartition == 0U ||
        media->BlockSize != 512U || media->IoAlign > PageBytes || m_block.Read == nullptr || m_block.Write == nullptr ||
        m_block.Flush == nullptr)
    {
        return false;
    }
    m_mediaId = media->MediaId;
    if (!Sector(0U) || m_sector[510U] != 0x55U || m_sector[511U] != 0xAAU || Read(m_sector + 11U, 2U) != 512U ||
        m_sector[16U] != 2U || Read(m_sector + 17U, 2U) != 0U || Read(m_sector + 22U, 2U) != 0U ||
        Read(m_sector + 40U, 2U) != 0U || Read(m_sector + 42U, 2U) != 0U)
    {
        return false;
    }
    m_clusterSectors = m_sector[13U];
    m_reserved = Read(m_sector + 14U, 2U);
    m_fatSectors = Read(m_sector + 36U, 4U);
    const UInt32 total{Read(m_sector + 32U, 4U)};
    const UInt32 root{Read(m_sector + 44U, 4U)};
    m_fsInfo = Read(m_sector + 48U, 2U);
    m_backupBoot = Read(m_sector + 50U, 2U);
    if (m_clusterSectors == 0U || m_clusterSectors > 128U || (m_clusterSectors & (m_clusterSectors - 1U)) != 0U ||
        m_reserved == 0U || m_fatSectors == 0U || total == 0U || UInt64{total - 1U} > media->LastBlock ||
        UInt64{m_reserved} + UInt64{m_fatSectors} * 2U >= total)
    {
        return false;
    }
    m_dataStart = m_reserved + m_fatSectors * 2U;
    m_clusters = (total - m_dataStart) / m_clusterSectors;
    UInt32 state{0U};
    if (m_clusters < 65525U || m_clusters > MaximumClusters || !Cluster(root) ||
        UInt64{m_clusters + 2U} * 4U > UInt64{m_fatSectors} * 512U || !GetFat(1U, state) ||
        (state & 0x0C00'0000U) != 0x0C00'0000U)
    {
        return false;
    }
    Entry entry{};
    if (!Find(root, "EFI", entry) || !entry.Found || (entry.Attributes & 0x10U) == 0U ||
        !Find(entry.Cluster, "ASTERIA", entry) || !entry.Found || (entry.Attributes & 0x10U) == 0U)
    {
        return false;
    }
    m_directory = entry.Cluster;
    if (!Find(m_directory, "ACPI", entry) || !entry.Found || (entry.Attributes & 0x10U) == 0U)
    {
        return false;
    }
    m_acpiDirectory = entry.Cluster;
    return true;
}
FilePresence Volume::Exists(const char* name) noexcept
{
    Entry entry{};
    if (!FindPath(name, entry))
    {
        return FilePresence::Failed;
    }
    return entry.Found ? FilePresence::Present : FilePresence::Missing;
}
bool Volume::InvalidateFsInfo() noexcept
{
    // Free counts/hints are optional. Mark both valid copies unknown before
    // allocating so a power loss cannot leave an overoptimistic free count.
    for (Size copy{0U}; copy < 2U; ++copy)
    {
        const UInt32 sector{m_fsInfo + (copy == 0U ? 0U : m_backupBoot)};
        if (m_fsInfo == 0U || sector >= m_reserved || (copy != 0U && m_backupBoot == 0U))
        {
            continue;
        }
        if (!Sector(sector))
        {
            return false;
        }
        if (Read(m_sector, 4U) == 0x4161'5252U && Read(m_sector + 484U, 4U) == 0x6141'7272U)
        {
            Write(m_sector + 488U, 0xFFFF'FFFFU, 4U);
            Write(m_sector + 492U, 0xFFFF'FFFFU, 4U);
            if (!Sector(sector, true))
            {
                return false;
            }
        }
    }
    return true;
}
bool Volume::Start(const char* name) noexcept
{
    if (m_open || m_broken || !FindPath(name, m_entry) || m_entry.Sector == 0U ||
        (m_entry.Found && (m_entry.Attributes & 0x11U) != 0U))
    {
        return false;
    }
    const char* leaf{name};
    if (name[0U] == 'A' && name[1U] == 'C' && name[2U] == 'P' && name[3U] == 'I' && name[4U] == '/')
    {
        leaf += 5U;
    }
    for (UInt8& byte : m_name)
    {
        byte = ' ';
    }
    Size cursor{0U};
    for (Size i{0U}; leaf[i] != '\0'; ++i)
    {
        if (leaf[i] == '.')
        {
            cursor = 8U;
        }
        else
        {
            m_name[cursor++] = static_cast<UInt8>(leaf[i]);
        }
    }
    m_first = 0U;
    m_last = 0U;
    m_chainSectors = 0U;
    m_bytes = 0U;
    m_buffered = 0U;
    UInt32 state{0U};
    m_open = GetFat(1U, state) && SetFat(1U, state & ~0x0800'0000U) && Flush() && InvalidateFsInfo();
    return m_open;
}
bool Volume::Drain() noexcept
{
    if (m_buffered == 0U)
    {
        return true;
    }
    if (m_chainSectors % m_clusterSectors == 0U)
    {
        UInt32 cluster{0U};
        if (!Allocate(cluster) || (m_last != 0U && !SetFat(m_last, cluster)))
        {
            return false;
        }
        if (m_first == 0U)
        {
            m_first = cluster;
        }
        m_last = cluster;
    }
    for (Size i{0U}; i < sizeof(m_sector); ++i)
    {
        m_sector[i] = i < m_buffered ? m_data[i] : 0U;
    }
    if (!Sector(ClusterSector(m_last) + m_chainSectors % m_clusterSectors, true))
    {
        return false;
    }
    ++m_chainSectors;
    m_buffered = 0U;
    return true;
}
bool Volume::Append(const char* bytes, const Size length) noexcept
{
    if (!m_open || m_broken || length > MaximumFileBytes - m_bytes)
    {
        m_broken = true;
        return false;
    }
    for (Size i{0U}; i < length; ++i)
    {
        m_data[m_buffered++] = static_cast<UInt8>(bytes[i]);
        if (m_buffered == sizeof(m_data) && !Drain())
        {
            m_broken = true;
            return false;
        }
    }
    m_bytes += length;
    return true;
}
bool Volume::FreeChain(UInt32 cluster) noexcept
{
    for (Size visited{0U}; Cluster(cluster) && visited < m_clusters; ++visited)
    {
        UInt32 next{0U};
        if (!GetFat(cluster, next) || !SetFat(cluster, 0U))
        {
            return false;
        }
        if (next >= 0x0FFF'FFF8U)
        {
            return true;
        }
        cluster = next;
    }
    return cluster == 0U;
}
bool Volume::Finish() noexcept
{
    if (!m_open)
    {
        return false;
    }
    m_open = false;
    if (m_broken || !Drain() || !Flush() || !Sector(m_entry.Sector))
    {
        return false;
    }
    UInt8* const entry{m_sector + m_entry.Offset};
    for (Size i{0U}; i < 32U; ++i)
    {
        entry[i] = 0U;
    }
    for (Size i{0U}; i < 11U; ++i)
    {
        entry[i] = m_name[i];
    }
    entry[11U] = 0x20U;
    Write(entry + 20U, m_first >> 16U, 2U);
    Write(entry + 26U, m_first & 0xFFFFU, 2U);
    Write(entry + 28U, static_cast<UInt32>(m_bytes), 4U);
    if (!Sector(m_entry.Sector, true) || !Flush())
    {
        return false;
    }
    UInt32 state{0U};
    return FreeChain(m_entry.Cluster) && Flush() && GetFat(1U, state) && SetFat(1U, state | 0x0800'0000U) && Flush();
}
}

FatReportVolume::FatReportVolume(BootServices& services) noexcept : m_services{services}
{
}
FatReportVolume::~FatReportVolume() noexcept
{
    static_cast<void>(Release());
}
IAcpiReportVolume* FatReportVolume::Open(const Handle device) noexcept
{
    if (m_address != 0U || m_services.AllocatePagesFunction == nullptr || m_services.FreePagesFunction == nullptr ||
        m_services.HandleProtocolFunction == nullptr)
    {
        return nullptr;
    }
    Guid guid{BlockGuid};
    void* interface{nullptr};
    if (m_services.HandleProtocolFunction(device, &guid, &interface) != Success || interface == nullptr ||
        m_services.AllocatePagesFunction(AllocateType::AnyPages, MemoryType::LoaderData, StoragePages, &m_address) !=
            Success)
    {
        m_address = 0U;
        return nullptr;
    }
    if (m_address == 0U || m_address % PageBytes != 0U)
    {
        static_cast<void>(m_services.FreePagesFunction(m_address, StoragePages));
        m_address = 0U;
        return nullptr;
    }
    // EFI pages are identity-addressable and aligned for both Volume and IO.
    auto* const volume = ::new (reinterpret_cast<void*>(m_address)) Volume{*static_cast<BlockIo*>(interface)};
    return volume->Mount() ? volume : nullptr;
}
bool FatReportVolume::Release() noexcept
{
    if (m_address == 0U)
    {
        return true;
    }
    auto* const volume{reinterpret_cast<Volume*>(m_address)};
    volume->~Volume();
    const UInt64 address{m_address};
    m_address = 0U;
    return m_services.FreePagesFunction(address, StoragePages) == Success;
}
}
