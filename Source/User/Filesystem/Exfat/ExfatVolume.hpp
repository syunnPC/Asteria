// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_EXFAT_EXFAT_VOLUME_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_EXFAT_EXFAT_VOLUME_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Filesystem/Names.hpp"

namespace Asteria::User::Filesystem
{
/// An exFAT volume of 512-byte sectors as the filesystem service's third backend: pure code over a
/// synchronous sector device, no system calls, no heap, with FatVolume's interface. The service
/// keeps one ExfatVolume static; every function's locals are small. A volume before its first Mount
/// is all zero bytes (Mount sets every field it reads), so a component image, which has no
/// initialized writable data, keeps it in .bss. Directories follow
/// their contiguous extent or FAT chain, and native UTF-8 names are converted to UTF-16 with
/// hashes/case folding from the authenticated up-case table. Identifiers pack parent cluster and
/// entry index; parent extent metadata remains private to the service. Reads exceed the
/// staged-write bound ExfatMaximumFilePages; COW writes retain contiguous runs. Writes are
/// copy-on-write per session, as on FAT. A session runs from a commit (or the mount) to the next
/// commit and changes at most one file. Its first page write reserves, in memory only, the first
/// run of free clusters that holds the committed size and at least six pages, extending its staging
/// capacity up to ExfatMaximumFilePages (Refused when the bitmap has no suitable run), and copies a
/// committed file into it; every page is written there at once. Commit writes, each write awaited:
/// the bitmap sectors that allocate the run's used prefix, the end mark's sector with its free
/// entries turned deleted when a created set lies in the sector after it, the entry set (one
/// sector: the point of no return), the bitmap sectors that free the replaced file's clusters, then
/// clears VolumeDirty. No FAT entry is ever written: a written file is NoFatChain, and the stale
/// FAT entries of a freed chain are unreferenced. A crash before the entry set leaves at worst
/// allocated clusters nothing references and VolumeDirty set; after it, the replaced file's
/// clusters still allocated. The library comes in two translation units: ExfatVolume.cpp holds the
/// read path (ReadExfatBootSector, Mount, Open, Stat, ReadPage) and ExfatVolumeWrite.cpp the write
/// path (Create, WritePage, Commit). The read unit references nothing the write unit defines, so a
/// read-only service links ExfatVolume.cpp alone and answers the write requests itself. VolumeDirty
/// (the main boot sector's VolumeFlags bit 1, outside the boot checksum; the backup boot region is
/// never written) is set at the session's first device write. A volume found dirty (or with
/// MediaFailure) mounts read-only; the volume never clears a dirty flag it did not set. A device
/// failure leaves the volume Broken: every later operation answers Failed until the next Mount.
inline constexpr Foundation::Size ExfatSectorBytes{512U};
inline constexpr Foundation::Size ExfatPageBytes{4096U};
inline constexpr Foundation::Size ExfatMaximumFilePages{MaximumWritePages};
inline constexpr Foundation::Size ExfatMaximumNameBytes{NameBytes};

/// The synchronous sector device a volume runs over, the shape of
/// FatBlockDevice (kept separate so neither library depends on the other).
/// ReadSectors and WriteSectors transfer count (1 to 8) sectors starting at
/// sector to or from buffer and answer whether they did; a false answer is a
/// device failure. ReadPage and WritePage pass the caller's page buffer (at
/// an offset of a multiple of 512 bytes) straight to the device; every other
/// transfer uses the volume's own sector buffer. Sectors is the device's
/// size.
struct ExfatBlockDevice
{
    void* Context;
    bool (*ReadSectors)(void* context, Foundation::UInt64 sector, Foundation::UInt32 count,
                        Foundation::UInt8* buffer) noexcept;
    bool (*WriteSectors)(void* context, Foundation::UInt64 sector, Foundation::UInt32 count,
                         const Foundation::UInt8* buffer) noexcept;
    Foundation::UInt64 Sectors;
    bool (*Synchronize)(void* context) noexcept {nullptr};
};

/// The outcome of an operation, FatStatus's values in its order; the first
/// seven match the file protocol's reply kinds (User/Component/FsFrame.hpp).
/// Refused: not executed, for the volume's state (unmounted, read-only,
/// another file's session, no free run, a full directory, a name that exists
/// or cannot be created, a file this library does not write) or a malformed
/// argument. NotExfat: sector 0 names no exFAT file system. Crashed: Commit
/// stopped at its crash point; the volume is Broken and the service is
/// expected to crash.
enum class ExfatStatus : Foundation::UInt8
{
    Done,
    Refused,
    Corrupt,
    StaleObject,
    NotFound,
    Failed,
    Unsupported,
    NotExfat,
    Crashed,
};

/// Commit's crash injection (the service's FsCrashBeforeSuperblock
/// and FsCrashAfterSuperblock): stop just before the entry set's write (after
/// the bitmap bits that allocate the new run), or just after it, before the
/// replaced clusters are freed and VolumeDirty cleared.
enum class ExfatCrashPoint : Foundation::UInt8
{
    None,
    BeforeDirectoryEntry,
    AfterDirectoryEntry,
};

/// A volume's layout decoded from its boot sector. Sector numbers are the
/// device's. Cluster numbers 2 to Clusters + 1 are the cluster heap's.
struct ExfatGeometry
{
    Foundation::UInt64 VolumeSectors;
    Foundation::UInt64 FatStart;
    Foundation::UInt64 FatSectors;
    Foundation::UInt64 HeapStart;
    Foundation::UInt32 Clusters;
    Foundation::UInt32 RootCluster;
    /// Log2 of the sectors in a cluster (0 to 16: clusters of at most 32 MiB).
    Foundation::UInt8 ClusterShift;
};

/// Decodes boot sector (512 bytes) of a device of deviceSectors sectors.
/// NotExfat, leaving geometry unchanged, unless its file system name (offset
/// 3) is "EXFAT ". Corrupt, also leaving geometry unchanged, unless it has
/// the boot signature, zeros in bytes 11 to 63, 512-byte sectors, clusters of
/// at most 32 MiB, one FAT (TexFAT's two are refused as unsupported),
/// revision 1.x, a volume within the device, a FAT from sector 24 on that
/// holds an entry per cluster, a cluster heap after it within the volume,
/// and a root directory cluster in the heap. The boot checksum, which spans
/// sectors 0 to 10, is Mount's to verify.
[[nodiscard]] ExfatStatus ReadExfatBootSector(const Foundation::UInt8* bootSector, Foundation::UInt64 deviceSectors,
                                              ExfatGeometry& geometry) noexcept;

/// A file's name: its File entry's index and the mount serial.
struct ExfatObject
{
    Foundation::UInt64 Identifier;
    Foundation::UInt64 Generation;
    Foundation::UInt64 ParentBytes{0U};
    bool ParentContiguous{false};
};

/// A file's size: Bytes, and Pages = Bytes rounded up to whole pages.
struct ExfatFileSize
{
    Foundation::UInt64 Pages;
    Foundation::UInt64 Bytes;
    NodeKind Kind{NodeKind::File};
};

class ExfatVolume final
{
public:
    constexpr ExfatVolume() noexcept = default;
    ExfatVolume(const ExfatVolume&) = delete;
    ExfatVolume& operator=(const ExfatVolume&) = delete;

    /// Mounts the volume on device, forgetting any earlier mount and its
    /// session. NotExfat or Corrupt as ReadExfatBootSector; Corrupt for a
    /// wrong boot checksum (sector 11's 128 copies over sectors 0 to 10,
    /// skipping VolumeFlags and PercentInUse), a root directory chain that
    /// leaves the heap, or no allocation bitmap entry that covers the heap;
    /// Failed for a device failure. A volume with VolumeDirty or
    /// MediaFailure set mounts read-only.
    [[nodiscard]] ExfatStatus Mount(const ExfatBlockDevice& device, Foundation::UInt64 serial) noexcept;

    [[nodiscard]] bool IsMounted() const noexcept;
    [[nodiscard]] bool IsReadOnly() const noexcept;
    [[nodiscard]] const ExfatGeometry& GetGeometry() const noexcept;

    /// Finds name (1 to ExfatMaximumNameBytes bytes) among the committed
    /// files of the root directory, matched in ASCII case-insensitively.
    /// Directories are not files. NotFound if absent; Corrupt when the entry
    /// set whose name matches is malformed.
    [[nodiscard]] ExfatStatus Open(const Foundation::UInt8* name, Foundation::Size length,
                                   ExfatObject& object) noexcept;

    /// Starts a session that creates name, an empty file, on the next Commit.
    /// The name follows the single UTF-8 name rules (Names.hpp) and is
    /// stored as given. Its entry set (File, Stream Extension and one File
    /// Name entry per 15 characters) is placed in the first run of free
    /// entries within one directory sector before the directory's end (the
    /// root never grows). Refused for a read-only volume, an open session, an
    /// invalid name, a name an entry set has, or no room in a sector.
    [[nodiscard]] ExfatStatus Create(const Foundation::UInt8* name, Foundation::Size length,
                                     ExfatObject& object) noexcept;

    /// The file's size, the session's for the session's file. StaleObject
    /// for a name of no file (another mount's serial, an index that holds no
    /// File entry of a file); Corrupt when the entry set at the index is
    /// malformed. ReadPage and WritePage resolve a name the same way.
    [[nodiscard]] ExfatStatus Stat(const ExfatObject& object, ExfatFileSize& size) noexcept;

    /// Reads the file's page into buffer (ExfatPageBytes), zeros past the
    /// valid data length. Refused for a page beyond the bounded 2 TiB range; Corrupt for clusters that do not hold the size.
    [[nodiscard]] ExfatStatus ReadPage(const ExfatObject& object, Foundation::UInt64 page,
                                       Foundation::UInt8* buffer) noexcept;

    /// Writes buffer (ExfatPageBytes) as the file's page, which must be below
    /// ExfatMaximumFilePages and at most the file's page count; the size
    /// becomes at least the end of the page (a partial last page that the
    /// write brings inside the size is zeroed past the old size first). The
    /// first write of a committed file starts its session and copies it to
    /// the reserved run. Refused for a read-only volume, while another file
    /// has a session, when no free run exists, and for a committed file this
    /// library does not write (larger than ExfatMaximumFilePages pages, a
    /// valid data length short of its data length, or a File and Stream
    /// Extension entry in different sectors), leaving the session as it was.
    [[nodiscard]] ExfatStatus WritePage(const ExfatObject& object, Foundation::UInt64 page,
                                        const Foundation::UInt8* buffer) noexcept;

    /// Makes the session durable in the order above and ends it (Done); with
    /// no session, clears a dirty flag this mount set. Crashed at crash's
    /// point, Failed for a device failure (both leave the volume Broken).
    [[nodiscard]] ExfatStatus Abort() noexcept;
    [[nodiscard]] ExfatStatus Commit(ExfatCrashPoint crash) noexcept;

    /// An exFAT volume keeps no transaction record.
    [[nodiscard]] static constexpr ExfatStatus QueryCommit() noexcept
    {
        return ExfatStatus::Unsupported;
    }

    [[nodiscard]] ExfatObject Root() const noexcept
    {
        return {RootIdentifier, m_serial};
    }
    [[nodiscard]] ExfatStatus Lookup(const ExfatObject& directory, const Foundation::UInt8* name,
                                     Foundation::Size length, ExfatObject& object) noexcept;
    [[nodiscard]] ExfatStatus CreateIn(const ExfatObject& directory, const Foundation::UInt8* name,
                                       Foundation::Size length, ExfatObject& object,
                                       bool directoryKind = false) noexcept;
    [[nodiscard]] ExfatStatus ReadDirectory(const ExfatObject& directory, Foundation::UInt64& cursor,
                                            Foundation::UInt8* name, Foundation::Size& length,
                                            ExfatObject& object) noexcept;

private:
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

    /// A file as Resolve finds it. Lba is the sector of its File entry; a
    /// chained file's Cursor-th cluster is CursorCluster (MapSector walks on
    /// from there, so a File's sectors are mapped in increasing order).
    struct File
    {
        Foundation::UInt64 Lba{};
        Foundation::UInt64 Size{};
        Foundation::UInt64 Valid{};
        Foundation::UInt32 First{};
        Foundation::UInt32 Cursor{};
        Foundation::UInt32 CursorCluster{};
        /// The File entry's SecondaryCount.
        Foundation::UInt8 Secondaries{};
        bool Contiguous{};
        /// The File and Stream Extension entries share a sector.
        bool Paired{};
        bool Session{};
        bool Directory{false};
    };

    /// One pass over the root directory (ScanDirectory), which decodes every
    /// entry set as it streams past, across sector and cluster boundaries.
    /// In: Name (nullptr to match nothing) and Length; Target, the index of
    /// a File entry whose set to decode (NoEntry for none); Run, the free
    /// entries wanted in one sector (0 for none). Out: Found, the first set
    /// whose name matches or that starts at Target, Match its file and
    /// Directory whether it is a directory, or Corrupt when that set is
    /// malformed; RunStart and RunLba, the first free run's first index and
    /// sector; End and EndLba, the index and sector of the end of the
    /// directory (End its capacity when it has none); Bitmap and
    /// BitmapBytes, the first allocation bitmap entry's.
    struct Scan
    {
        const Foundation::UInt8* Name{};
        Foundation::Size Length{};
        Foundation::UInt32 Target{};
        Foundation::UInt32 Run{};
        Foundation::UInt32 Found{};
        Foundation::UInt32 RunStart{};
        Foundation::UInt32 End{};
        Foundation::UInt32 Bitmap{};
        Foundation::UInt64 RunLba{};
        Foundation::UInt64 EndLba{};
        Foundation::UInt64 BitmapBytes{};
        File Match{};
        bool Directory{};
        bool Corrupt{};
        bool Enumerate{false};
        Foundation::UInt32 Cursor{0U};
        Foundation::UInt8* Output{nullptr};
        Foundation::Size LengthOut{0U};
        Foundation::UInt32 Upcase{0U};
        Foundation::UInt64 UpcaseBytes{0U};
        Foundation::UInt32 UpcaseChecksum{0U};
    };

    // Read path (ExfatVolume.cpp).
    [[nodiscard]] ExfatStatus SelectDirectory(const ExfatObject& directory) noexcept;
    [[nodiscard]] ExfatStatus DirectorySector(Foundation::UInt32 ordinal, Foundation::UInt32& cluster,
                                              Foundation::UInt64& lba) noexcept;
    [[nodiscard]] ExfatStatus InitializeDirectory() noexcept;
    [[nodiscard]] ExfatStatus ReadUpcaseSector(Foundation::UInt32 ordinal, Foundation::UInt32& cluster) noexcept;
    [[nodiscard]] ExfatStatus VerifyUpcase() noexcept;
    [[nodiscard]] ExfatStatus Upper(Foundation::UInt16 unit, Foundation::UInt16& upper) noexcept;
    [[nodiscard]] ExfatStatus Fail() noexcept;
    [[nodiscard]] ExfatStatus Usable() const noexcept;
    [[nodiscard]] ExfatStatus Load(Foundation::UInt64 lba) noexcept;
    [[nodiscard]] ExfatStatus GetEntry(Foundation::UInt32 cluster, Foundation::UInt32& value) noexcept;
    [[nodiscard]] ExfatStatus NextSector(Foundation::UInt32 ordinal, Foundation::UInt32& cluster,
                                         Foundation::UInt64& lba) noexcept;
    [[nodiscard]] ExfatStatus ScanDirectory(Scan& scan) noexcept;
    [[nodiscard]] ExfatStatus Resolve(const ExfatObject& object, File& file) noexcept;
    [[nodiscard]] ExfatStatus MapSector(File& file, Foundation::UInt32 fileSector, Foundation::UInt64& lba) noexcept;
    [[nodiscard]] ExfatStatus Transfer(File& file, Foundation::UInt64 page, Foundation::UInt32 sectors,
                                       Foundation::UInt8* readBuffer, const Foundation::UInt8* writeBuffer) noexcept;
    [[nodiscard]] bool IsCluster(Foundation::UInt32 value) const noexcept;
    [[nodiscard]] Foundation::UInt64 ClusterSector(Foundation::UInt32 cluster) const noexcept;
    [[nodiscard]] File SessionFile() const noexcept;

    // Write path (ExfatVolumeWrite.cpp).
    [[nodiscard]] ExfatStatus Store(Foundation::UInt64 lba) noexcept;
    [[nodiscard]] ExfatStatus ChainSector(Foundation::UInt32 first, Foundation::UInt32 ordinal,
                                          Foundation::UInt64& lba) noexcept;
    [[nodiscard]] ExfatStatus SetDirty(bool dirty) noexcept;
    [[nodiscard]] ExfatStatus Reserve(Foundation::UInt32 minimum = 0U) noexcept;
    [[nodiscard]] ExfatStatus SetBits(Foundation::UInt32 first, Foundation::UInt32 count, bool allocated) noexcept;
    [[nodiscard]] ExfatStatus FreeOld() noexcept;
    [[nodiscard]] ExfatStatus StartReplace(File& file, Foundation::UInt64 index, Foundation::UInt64 page) noexcept;
    [[nodiscard]] ExfatStatus ZeroTail() noexcept;
    void PrepareSet() noexcept;
    [[nodiscard]] ExfatStatus WriteEntrySet() noexcept;

    ExfatBlockDevice m_device{};
    ExfatGeometry m_geometry{};
    Foundation::UInt64 m_serial{0U};
    /// The sector m_sector holds, NoSector when none (from Mount on).
    Foundation::UInt64 m_cached{0U};
    /// The allocation bitmap's first cluster, set by Mount.
    Foundation::UInt32 m_bitmap{0U};
    /// The session's file: its File entry's sector and index, size, and the
    /// entries of its set; a created set's end-mark sector to mark deleted
    /// from m_gapSlot on (0 for none).
    Foundation::UInt64 m_sessionLba{0U};
    Foundation::UInt64 m_gapLba{0U};
    Foundation::UInt64 m_sessionIndex{0U};
    File m_directory{};
    File m_sessionParent{};
    bool m_directoryKind{false};
    Foundation::UInt32 m_upcase{0U};
    Foundation::UInt32 m_upcaseChecksum{0U};
    bool m_upcaseVerified{false};
    Foundation::UInt8 m_upperCount{0U};
    struct UpperPair
    {
        Foundation::UInt16 Lower;
        Foundation::UInt16 Upper;
    };
    UpperPair m_upperCache[16U]{};
    Foundation::UInt64 m_upcaseBytes{0U};
    Foundation::UInt32 m_nameHash{0U};
    Foundation::UInt32 m_sessionSize{0U};
    /// The reserved run (m_runLength 0 until the session's first write; no
    /// run outside a session).
    Foundation::UInt32 m_runFirst{0U};
    Foundation::UInt32 m_runLength{0U};
    /// The replaced file's clusters, freed after the entry set.
    Foundation::UInt32 m_oldFirst{0U};
    Foundation::UInt32 m_oldClusters{0U};
    State m_state{State::Unmounted};
    Session m_session{Session::None};
    bool m_readOnly{false};
    bool m_dirtySet{false};
    bool m_zeroTail{false};
    bool m_oldContiguous{false};
    Foundation::UInt8 m_slot{0U};
    Foundation::UInt8 m_gapSlot{0U};
    Foundation::UInt8 m_setEntries{0U};
    Foundation::UInt8 m_nameLength{0U};
    Foundation::UInt16 m_name[ExfatMaximumNameBytes]{};
    Foundation::UInt16 m_scanName[255U]{};
    Foundation::UInt16 m_matchName[NameBytes]{};
    Foundation::UInt8 m_rendered[NameBytes]{};
    // Separate scratch for up-case decoding so scanning never loses its sector.
    Foundation::UInt8 m_upcaseSector[ExfatSectorBytes]{};
    Foundation::UInt8 m_sector[ExfatSectorBytes]{};
};
} // Namespace Asteria::User::Filesystem

#endif // ASTERIA_USER_FILESYSTEM_EXFAT_EXFAT_VOLUME_HPP_INCLUDED
