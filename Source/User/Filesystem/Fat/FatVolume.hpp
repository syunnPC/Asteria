// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_FAT_FAT_VOLUME_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_FAT_FAT_VOLUME_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Filesystem/Names.hpp"

namespace Asteria::User::Filesystem
{
/// A FAT12, FAT16 or FAT32 volume of 512-byte sectors as the filesystem service's FAT backend: pure
/// code over a synchronous sector device, no system calls, no heap. The service keeps one FatVolume
/// static; every function's locals are small. A volume before its first Mount is all zero bytes
/// (Mount sets every field it reads), so a component image, which has no initialized writable data,
/// keeps it in .bss. Supports cluster-backed subdirectories and files beyond six
/// pages. Reads cover the FAT file-size range; COW staging is bounded by FatMaximumFilePages.
/// Identifiers pack parent first cluster and short-entry index; Generation is the service's mount
/// serial. Writes are copy-on-write per session. A session runs from a commit (or the mount)
/// to the next commit and changes at most one file. A created file's clusters are fresh; the first
/// WritePage of a committed file copies the file to fresh clusters, and the session's later writes
/// go there. Data is written at once; FAT changes are staged as that new chain. Commit writes, each
/// write awaited: every changed FAT sector to copy 0 then copy 1, FSInfo on FAT32, the directory
/// entry set (one sector: the point of no return; a created set that starts the sector after the
/// directory's end mark first turns that mark's sector's free entries into deleted ones), the freed
/// old chain and FSInfo again when a committed file was replaced, then clears the dirty state.
/// Every FAT write before the directory entry touches clusters nothing references yet and every one
/// after it clusters nothing references any more, so a crash leaves at worst lost clusters, a stale
/// FSInfo free count, the dirty state, or (between the two copies of one sector) FATs that differ
/// in that sector. On FAT12 an entry that straddles two FAT sectors is written in the order that
/// keeps its half-written value free, an end mark or a cluster number. The dirty state is set at
/// the session's first device write: FAT[1]'s clean bit (bit 15 on FAT16, bit 27 on FAT32) cleared
/// in every FAT, on FAT12 bit 0 of the boot sector's byte 0x25. A volume found dirty (either mark,
/// or a FAT[1] hard-error bit) or with a single active FAT32 FAT mounts read-only; the volume never
/// clears a dirty state it did not set. A device failure leaves the volume Broken: every later
/// operation answers Failed until the next Mount.
inline constexpr Foundation::Size FatSectorBytes{512U};
inline constexpr Foundation::Size FatPageBytes{4096U};
inline constexpr Foundation::Size FatMaximumFilePages{MaximumWritePages};
inline constexpr Foundation::Size FatMaximumNameBytes{NameBytes};

/// The synchronous sector device a volume runs over. ReadSectors and
/// WriteSectors transfer count (1 to 8) sectors starting at sector to or from
/// buffer and answer whether they did; a false answer is a device failure.
/// ReadPage and WritePage pass the caller's page buffer (at an offset of a
/// multiple of 512 bytes) straight to the device; every other transfer uses
/// the volume's own sector buffer. Sectors is the device's size.
struct FatBlockDevice
{
    void* Context;
    bool (*ReadSectors)(void* context, Foundation::UInt64 sector, Foundation::UInt32 count,
                        Foundation::UInt8* buffer) noexcept;
    bool (*WriteSectors)(void* context, Foundation::UInt64 sector, Foundation::UInt32 count,
                         const Foundation::UInt8* buffer) noexcept;
    Foundation::UInt64 Sectors;
    bool (*Synchronize)(void* context) noexcept {nullptr};
};

enum class FatType : Foundation::UInt8
{
    Fat12 = 12U,
    Fat16 = 16U,
    Fat32 = 32U,
};

/// The outcome of an operation; the first seven match the file protocol's
/// reply kinds (User/Component/FsFrame.hpp). Refused: not executed, for the
/// volume's state (unmounted, read-only, another file's session, a full
/// volume or directory, a name that exists or cannot be created) or a
/// malformed argument. NotFat: Mount found no FAT parameter block (
/// sanity). Crashed: Commit stopped at its crash point; the volume is Broken
/// and the service is expected to crash.
enum class FatStatus : Foundation::UInt8
{
    Done,
    Refused,
    Corrupt,
    StaleObject,
    NotFound,
    Failed,
    Unsupported,
    NotFat,
    Crashed,
};

/// Commit's crash injection (FsCrashBeforeSuperblock and
/// FsCrashAfterSuperblock on FAT): stop just before the directory
/// entry set's write, or just after it, before the old chain is freed and the
/// dirty state cleared.
enum class FatCrashPoint : Foundation::UInt8
{
    None,
    BeforeDirectoryEntry,
    AfterDirectoryEntry,
};

/// A volume's layout decoded from its boot sector. Sector numbers are the
/// device's. Clusters numbers 2 to Clusters + 1 are data clusters.
struct FatGeometry
{
    /// The first sector of the FAT the volume reads (FAT copy 0, or on FAT32
    /// with mirroring disabled the active FAT).
    Foundation::UInt64 FatStart;
    Foundation::UInt64 FatSectors;
    /// FAT12 and FAT16: the fixed root directory; zero on FAT32.
    Foundation::UInt64 RootStart;
    Foundation::UInt64 DataStart;
    Foundation::UInt32 Clusters;
    /// FAT12 and FAT16: the root directory's entries.
    Foundation::UInt32 RootEntries;
    /// FAT32: the root directory's first cluster.
    Foundation::UInt32 RootCluster;
    /// FAT32: the FSInfo sector, zero when the volume names none (after Mount
    /// also when its signatures are wrong; FSInfo is then never written).
    Foundation::UInt32 FsInfoSector;
    /// Log2 of the sectors in a cluster (0 to 7).
    Foundation::UInt8 ClusterShift;
    Foundation::UInt8 FatCount;
    FatType Type;
    /// FAT32 ExtFlags bit 7: one active FAT, no mirroring.
    bool SingleActiveFat;
};

/// Decodes boot sector (512 bytes) of a device of deviceSectors sectors.
/// NotFat, leaving geometry unchanged, unless it has a jump instruction,
/// 512-byte sectors, a power-of-two cluster of at most 128 sectors, at least
/// one reserved sector, one or two FATs, a sector count within the device, a
/// FAT size and a data area. Corrupt, also leaving geometry unchanged, when the
/// 16-bit FAT size disagrees with the type the cluster count gives (zero
/// exactly on FAT32), a FAT cannot hold an entry per cluster, a FAT12 or FAT16
/// volume has no root entries, or a FAT32 volume has root entries, a root
/// cluster out of range, or more clusters than its entries can number.
[[nodiscard]] FatStatus ReadFatGeometry(const Foundation::UInt8* bootSector, Foundation::UInt64 deviceSectors,
                                        FatGeometry& geometry) noexcept;

/// The checksum of an 11-byte short name that its long-name entries carry.
[[nodiscard]] Foundation::UInt8 ComputeFatNameChecksum(const Foundation::UInt8* shortName) noexcept;

/// Renders a 32-byte short directory entry's name as "BASE.EXT" into name
/// (trailing spaces dropped, no '.' without an extension, a leading 0x05 as
/// 0xE5), lowering the base and the extension when the entry's NT case flags
/// (byte 12: 0x08, 0x10) say so; returns its length, 0 to 12.
Foundation::Size RenderFatShortName(const Foundation::UInt8* entry, Foundation::UInt8 (&name)[12]) noexcept;

/// A file's name: its short entry's index and the mount serial.
struct FatObject
{
    Foundation::UInt64 Identifier;
    Foundation::UInt64 Generation;
};

/// A file's size: Bytes, and Pages = Bytes rounded up to whole pages.
struct FatFileSize
{
    Foundation::UInt64 Pages;
    Foundation::UInt64 Bytes;
    NodeKind Kind{NodeKind::File};
};

class FatVolume final
{
public:
    constexpr FatVolume() noexcept = default;
    FatVolume(const FatVolume&) = delete;
    FatVolume& operator=(const FatVolume&) = delete;

    /// Mounts the volume on device, forgetting any earlier mount and its
    /// session. NotFat or Corrupt as ReadFatGeometry, or Corrupt for a FAT32
    /// root chain that leaves the volume; Failed for a device failure. A dirty
    /// volume or a FAT32 volume with a single active FAT mounts read-only.
    [[nodiscard]] FatStatus Mount(const FatBlockDevice& device, Foundation::UInt64 serial) noexcept;

    [[nodiscard]] bool IsMounted() const noexcept;
    [[nodiscard]] bool IsReadOnly() const noexcept;
    [[nodiscard]] const FatGeometry& GetGeometry() const noexcept;

    /// Finds name (1 to FatMaximumNameBytes bytes) among the committed files
    /// of the root directory: matched in ASCII case-insensitively against a
    /// file's long name when it has a well-formed one, else its 8.3 name.
    /// Directories and the volume label are not files. NotFound if absent.
    [[nodiscard]] FatStatus Open(const Foundation::UInt8* name, Foundation::Size length, FatObject& object) noexcept;

    /// Starts a session that creates name, an empty file, on the next Commit.
    /// The name follows the single UTF-8 name rules (Names.hpp). A name that
    /// fits 8.3 becomes an uppercase short entry with the NT case flags; a
    /// longer one long-name entries and a short name BASIS~N (N 1 to 9), the
    /// whole set within one directory sector. Refused for a read-only volume,
    /// an open session, an invalid or DOS device name, a name a file has, a
    /// short name that collides, no free N, or no room in a sector.
    [[nodiscard]] FatStatus Create(const Foundation::UInt8* name, Foundation::Size length, FatObject& object) noexcept;

    /// The file's size, the session's for the session's file.
    [[nodiscard]] FatStatus Stat(const FatObject& object, FatFileSize& size) noexcept;

    /// Reads the file's page into buffer (FatPageBytes), zeros past the size.
    /// Refused for a page beyond the FAT 32-bit byte-size range; StaleObject for a
    /// name of no file; Corrupt for a chain that does not hold the size.
    [[nodiscard]] FatStatus ReadPage(const FatObject& object, Foundation::UInt64 page,
                                     Foundation::UInt8* buffer) noexcept;

    /// Writes buffer (FatPageBytes) as the file's page, which must be below
    /// FatMaximumFilePages and at most the file's page count; the size becomes
    /// at least the end of the page (a partial last page that the write brings
    /// inside the size is zeroed past the old size first, so no stale byte
    /// becomes the file's). The first write of a committed file
    /// starts its session (Refused while another file has one, or for a file
    /// larger than FatMaximumFilePages pages) and copies it to fresh clusters.
    /// Refused for a read-only volume or a full one, leaving the session as it
    /// was.
    [[nodiscard]] FatStatus WritePage(const FatObject& object, Foundation::UInt64 page,
                                      const Foundation::UInt8* buffer) noexcept;

    /// Makes the session durable in the order above and ends it (Done); with
    /// no session, clears a dirty state this mount set. Crashed at crash's
    /// point, Failed for a device failure (both leave the volume Broken).
    [[nodiscard]] FatStatus Abort() noexcept;
    [[nodiscard]] FatStatus Commit(FatCrashPoint crash) noexcept;

    /// A FAT volume keeps no transaction record.
    [[nodiscard]] static constexpr FatStatus QueryCommit() noexcept
    {
        return FatStatus::Unsupported;
    }

    [[nodiscard]] FatObject Root() const noexcept
    {
        return {RootIdentifier, m_serial};
    }
    [[nodiscard]] FatStatus Lookup(const FatObject& directory, const Foundation::UInt8* name, Foundation::Size length,
                                   FatObject& object) noexcept;
    [[nodiscard]] FatStatus CreateIn(const FatObject& directory, const Foundation::UInt8* name, Foundation::Size length,
                                     FatObject& object, bool directoryKind = false) noexcept;
    [[nodiscard]] FatStatus ReadDirectory(const FatObject& directory, Foundation::UInt64& cursor,
                                          Foundation::UInt8* name, Foundation::Size& length,
                                          FatObject& object) noexcept;

private:
    static constexpr Foundation::Size MaximumChain{(FatMaximumFilePages * FatPageBytes) / FatSectorBytes};

    enum class State : Foundation::UInt8
    {
        Unmounted,
        Mounted,
        Broken,
    };

    enum class Session : Foundation::UInt8
    {
        None,
        Create,
        Replace,
    };

    struct File
    {
        Foundation::UInt64 Lba;
        Foundation::UInt32 First;
        Foundation::UInt32 Size;
        bool Session;
        bool Directory{false};
    };

    struct Scan;

    [[nodiscard]] FatStatus SelectDirectory(const FatObject& directory) noexcept;
    [[nodiscard]] FatStatus GrowDirectory() noexcept;
    [[nodiscard]] FatStatus InitializeDirectory(Foundation::UInt32 parent, Foundation::UInt32 fresh) noexcept;
    [[nodiscard]] FatStatus Fail() noexcept;
    [[nodiscard]] FatStatus Usable() const noexcept;
    [[nodiscard]] FatStatus Load(Foundation::UInt64 lba) noexcept;
    [[nodiscard]] FatStatus Store(Foundation::UInt64 lba) noexcept;
    [[nodiscard]] FatStatus Flush() noexcept;
    [[nodiscard]] FatStatus GetEntry(Foundation::UInt32 cluster, Foundation::UInt32& value) noexcept;
    [[nodiscard]] FatStatus SetEntry(Foundation::UInt32 cluster, Foundation::UInt32 value) noexcept;
    [[nodiscard]] FatStatus Entry(Foundation::UInt32 cluster, Foundation::UInt32& value, bool write) noexcept;
    [[nodiscard]] FatStatus Allocate(Foundation::UInt32& cluster) noexcept;
    [[nodiscard]] FatStatus SetDirty(bool dirty) noexcept;
    [[nodiscard]] FatStatus WriteFsInfo() noexcept;
    [[nodiscard]] FatStatus WriteDirectory() noexcept;
    [[nodiscard]] FatStatus FreeOldChain() noexcept;
    [[nodiscard]] FatStatus NextSector(Foundation::UInt32 ordinal, Foundation::UInt32& cluster,
                                       Foundation::UInt64& lba) noexcept;
    [[nodiscard]] FatStatus ScanDirectory(Scan& scan) noexcept;
    [[nodiscard]] FatStatus Resolve(const FatObject& object, File& file) noexcept;
    [[nodiscard]] FatStatus MapSector(const File& file, Foundation::UInt32 fileSector,
                                      Foundation::UInt64& lba) noexcept;
    [[nodiscard]] FatStatus Transfer(const File& file, Foundation::UInt64 page, Foundation::UInt32 sectors,
                                     Foundation::UInt8* readBuffer, const Foundation::UInt8* writeBuffer) noexcept;
    [[nodiscard]] FatStatus StartReplace(const File& file, Foundation::UInt64 index, Foundation::UInt64 page) noexcept;
    [[nodiscard]] FatStatus ZeroTail() noexcept;
    [[nodiscard]] bool IsCluster(Foundation::UInt32 value) const noexcept;
    [[nodiscard]] bool InSession(Foundation::UInt32 cluster) const noexcept;
    [[nodiscard]] Foundation::UInt64 ClusterSector(Foundation::UInt32 cluster) const noexcept;

    FatBlockDevice m_device{};
    FatGeometry m_geometry{};
    Foundation::UInt64 m_serial{0U};
    /// The sector m_sector holds, NoSector when none (from Mount on);
    /// m_dirtyFat marks it a changed FAT copy-0 sector not yet written.
    Foundation::UInt64 m_cached{0U};
    /// The session's directory sector and its file's short entry index; a
    /// created set's end-mark sector to mark deleted from m_gapSlot on (0 for
    /// none).
    Foundation::UInt64 m_sessionLba{0U};
    Foundation::UInt64 m_gapLba{0U};
    Foundation::UInt64 m_sessionIndex{0U};
    Foundation::UInt32 m_directory{0U};
    bool m_directoryKind{false};
    Foundation::UInt32 m_sessionSize{0U};
    /// The replaced file's chain, freed after the directory entry.
    Foundation::UInt32 m_oldFirst{0U};
    Foundation::UInt32 m_oldClusters{0U};
    /// Entries before m_end are the directory's (its end mark or capacity).
    Foundation::UInt32 m_end{0U};
    /// FAT32 FSInfo: the free count (~0 unknown) and where a search starts,
    /// both set by Mount.
    Foundation::UInt32 m_freeCount{0U};
    Foundation::UInt32 m_hint{0U};
    /// The session's chain: fresh clusters in file order.
    Foundation::UInt32 m_chain[MaximumChain]{};
    Foundation::UInt32 m_chainLength{0U};
    State m_state{State::Unmounted};
    Session m_session{Session::None};
    bool m_readOnly{false};
    bool m_dirtyFat{false};
    bool m_dirtySet{false};
    bool m_zeroTail{false};
    /// A created file: its long-name entries (0 to 5), case flags, short name
    /// and name; m_slot is the set's first entry within its sector.
    Foundation::UInt8 m_slot{0U};
    Foundation::UInt8 m_gapSlot{0U};
    Foundation::UInt8 m_longEntries{0U};
    Foundation::UInt8 m_caseFlags{0U};
    Foundation::UInt8 m_nameLength{0U};
    Foundation::UInt8 m_shortName[11]{};
    Foundation::UInt16 m_name[FatMaximumNameBytes]{};
    // FAT specification 1.03, "FAT Long Directory Entries": 20 x 13 units.
    Foundation::UInt16 m_scanName[260U]{};
    Foundation::UInt8 m_rendered[NameBytes]{};
    static_assert(MaximumChain == MaximumWritePages * (FatPageBytes / FatSectorBytes));
    Foundation::UInt8 m_sector[FatSectorBytes]{};
};
} // Namespace Asteria::User::Filesystem

#endif // ASTERIA_USER_FILESYSTEM_FAT_FAT_VOLUME_HPP_INCLUDED
