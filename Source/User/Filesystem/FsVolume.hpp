// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_FS_VOLUME_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_FS_VOLUME_HPP_INCLUDED

#include "User/Filesystem/FsFormat.hpp"

namespace Asteria::User::Filesystem
{
/// Callbacks borrow the buffer for the duration of the call. Each write must
/// finish before returning; a false result may still mean bytes landed. The
/// device must be quiescent before remount (no late writes from an old caller).
struct FsBlockDevice
{
    void* Context;
    bool (*ReadSectors)(void*, UInt64 sector, UInt32 count, UInt8* buffer) noexcept;
    bool (*WriteSectors)(void*, UInt64 sector, UInt32 count, const UInt8* buffer) noexcept;
};

enum class FsStatus : UInt8
{
    Success,
    Blank,
    Corrupt,
    IoError,
    NotMounted,
    InvalidName,
    AlreadyExists,
    NotFound,
    StaleObject,
    Full,
    InvalidIndex,
    InvalidObject,
    GenerationExhausted,
    Committed,
    NotCommitted,
};
struct FsHandle
{
    UInt64 ObjectId{0U};
    UInt64 BirthGeneration{0U};
};
struct FsHandleResult
{
    FsStatus Status;
    FsHandle Handle;
};
struct FsStatResult
{
    FsStatus Status;
    FsObject Object;
};
struct FsAllocationResult
{
    FsStatus Status;
    UInt32 Block;
};
struct FsCommitResult
{
    FsStatus Status;
    UInt64 Generation;
};

/// One serialized staging transaction over fixed storage. The service owns
/// sender admission/authorization and forwards whole fresh data blocks, awaiting
/// their successful Done before Commit. This library never reads/writes data.
/// Mount discards staging; Blank requires an explicit Format. Failure of any
/// commit write fences this instance until Mount; query only after remount.
/// Ordering guarantees service-crash consistency, not power-loss durability
/// without a device flush. Keep the volume static (about 13 KiB), not on a
/// service's 512-byte stack. Buffers and candidate superblocks are members.
/// A volume before its first Mount or Format is all zero bytes, so a
/// component image, which has no initialized writable data, keeps it in.bss.
class FsVolume final
{
public:
    constexpr FsVolume() noexcept = default;
    FsVolume(const FsVolume&) = delete;
    FsVolume& operator=(const FsVolume&) = delete;
    FsVolume(FsVolume&&) = delete;
    FsVolume& operator=(FsVolume&&) = delete;

    [[nodiscard]] FsStatus Mount(FsBlockDevice device) noexcept;
    /// Refuses any nonzero superblock sector, even if it decodes successfully.
    [[nodiscard]] FsStatus Format(FsBlockDevice device) noexcept;
    [[nodiscard]] FsHandleResult Create(const UInt8* name, Size size) noexcept;
    [[nodiscard]] FsHandleResult Lookup(const UInt8* name, Size size) const noexcept;
    [[nodiscard]] FsStatus ReadDirectory(UInt64& cursor, UInt8* name, Size& length, FsHandle& handle) const noexcept;
    [[nodiscard]] FsStatResult Stat(FsHandle handle) const noexcept;
    /// Returns a new full block; replaces Direct[index] and extends SizeBytes
    /// to its end. Sparse holes are zero pointers. Old blocks stay unavailable
    /// until the publishing superblock write succeeds. Replacements staged
    /// within one transaction are also retained until commit/remount.
    [[nodiscard]] FsAllocationResult AllocateData(FsHandle handle, Size index) noexcept;
    /// Keeps the last four successful commit records, newest first. Replaying
    /// a retained pair returns its original generation without any write or
    /// change to staging. Callers must not reuse a pair for a different commit.
    [[nodiscard]] FsCommitResult Commit(UInt64 sender, UInt64 transaction) noexcept;
    /// PowerOff.md / filesystem protocol: publish pending staging without consulting or changing
    /// client commit history. A clean volume writes nothing; the service must still flush this
    /// generation before replying.
    [[nodiscard]] FsCommitResult CommitStaged() noexcept;
    /// Definite status within the four-record retention window. Evicted pairs
    /// are NotCommitted; the service must bound its retry/query lifetime.
    [[nodiscard]] FsCommitResult QueryCommit(UInt64 sender, UInt64 transaction) const noexcept;
    [[nodiscard]] UInt64 GetGeneration() const noexcept;
    /// Valid only after successful Mount/Format. Bitmap includes permanently
    /// unavailable reserved blocks 0-15, plus exactly the reachable CoW blocks.
    [[nodiscard]] const FsSuperblock& GetCommittedSuperblock() const noexcept;

private:
    [[nodiscard]] FsStatus ReadSuperblocks(FsBlockDevice device) noexcept;
    [[nodiscard]] FsStatus LoadCandidate(Size slot) noexcept;
    [[nodiscard]] bool ValidateReachable(const FsSuperblock& superblock) noexcept;
    [[nodiscard]] bool AddReachable(UInt32 block) noexcept;
    [[nodiscard]] UInt32 FindFreshBlock() noexcept;
    [[nodiscard]] FsStatus ValidateHandle(FsHandle handle) const noexcept;
    [[nodiscard]] FsCommitResult Publish(UInt64 sender, UInt64 transaction, bool recordCommit) noexcept;
    [[nodiscard]] bool WriteBlock(UInt32 block) noexcept;
    static void ReserveBitmap(UInt8* bitmap) noexcept;
    [[nodiscard]] static int CompareName(const UInt8* left, Size leftSize, const UInt8* right, Size rightSize) noexcept;

    FsBlockDevice m_device{.Context = nullptr, .ReadSectors = nullptr, .WriteSectors = nullptr};
    FsSuperblock m_superblocks[2U]{};
    FsSuperblock m_candidate{};
    FsObject m_objects[FsObjectCapacity]{};
    FsDirectoryEntry m_directory[FsDirectoryCapacity]{};
    UInt8 m_buffer[FsBlockBytes]{};
    UInt8 m_allocated[FsBitmapBytes]{};
    UInt8 m_reachable[FsBitmapBytes]{};
    UInt64 m_nextObjectId{0U};
    Size m_directoryCount{0U};
    Size m_selected{0U};
    bool m_valid[2U]{};
    bool m_blank[2U]{};
    bool m_mounted{false};
    bool m_staged{false};
};
} // Namespace Asteria::User::Filesystem

#endif // ASTERIA_USER_FILESYSTEM_FS_VOLUME_HPP_INCLUDED
