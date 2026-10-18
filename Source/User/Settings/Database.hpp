// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SETTINGS_DATABASE_HPP_INCLUDED
#define ASTERIA_USER_SETTINGS_DATABASE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Settings
{
// SettingsDatabase.md records the format, validation and storage contracts.
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
inline constexpr Size KeyCapacity{32U};
inline constexpr Size MaximumDepth{8U};
inline constexpr Size MaximumNameBytes{63U};
inline constexpr Size MaximumValueBytes{256U};
inline constexpr Size AclCapacity{8U};
inline constexpr Size ValueCapacity{64U};
inline constexpr Size ValuesPerKey{8U};
inline constexpr Size WatchCapacity{8U};
inline constexpr Size QueueCapacity{8U};
inline constexpr Size MaximumPathBytes{MaximumDepth * (MaximumNameBytes + 1U)};
inline constexpr UInt8 ReadRight{1U};
inline constexpr UInt8 WriteRight{2U};
inline constexpr UInt8 CreateRight{4U};
inline constexpr UInt8 DeleteRight{8U};
inline constexpr UInt8 ChangeAclRight{16U};
inline constexpr UInt8 AllRights{ReadRight | WriteRight | CreateRight | DeleteRight | ChangeAclRight};
inline constexpr Size AclRecordBytes{12U};
inline constexpr Size KeyRecordBytes{8U + MaximumNameBytes + AclCapacity * AclRecordBytes};
inline constexpr Size HeaderBytes{32U};
inline constexpr Size SealBytes{32U};
inline constexpr Size SlotBytes{32768U};
inline constexpr Size ValueRecordBytes{8U + MaximumNameBytes + MaximumValueBytes};
inline constexpr Size ValueTableOffset{HeaderBytes + KeyCapacity * KeyRecordBytes};
inline constexpr Size BodyBytes{ValueTableOffset + ValueCapacity * ValueRecordBytes};
inline constexpr Size StorageBytes{2U * SlotBytes};
inline constexpr Size SealOffset{SlotBytes - SealBytes};
static_assert(BodyBytes <= SealOffset);
static_assert(KeyCapacity <= 256U && MaximumNameBytes <= 255U && MaximumValueBytes <= 65535U);
static_assert(MaximumPathBytes == 512U && KeyRecordBytes == 167U && ValueRecordBytes == 327U && BodyBytes == 26304U);

struct TextView
{
    const UInt8* Bytes{nullptr};
    Size Length{0U};
};
struct Name
{
    UInt8 Bytes[MaximumNameBytes]{};
    Size Length{0U};
};
struct Path
{
    UInt8 Bytes[MaximumPathBytes]{};
    Size Length{0U};
};
enum class ValueType : UInt8
{
    None,
    Integer,
    Boolean,
    String,
    Binary,
    StringList
};
struct Value
{
    ValueType Type{ValueType::None};
    UInt16 Length{0U};
    UInt8 Bytes[MaximumValueBytes]{};
};
struct AclEntry
{
    UInt64 Principal{0U};
    UInt8 Rights{0U};
    bool Deny{false};
    bool Inherit{false};
};
struct Access
{
    UInt64 Principal{0U};
    UInt8 ViewRights{0U};
};
struct Key
{
    bool Used{false};
    UInt8 Parent{0U};
    UInt8 NameLength{0U};
    UInt8 Name[MaximumNameBytes]{};
    UInt8 AclCount{0U};
    AclEntry Acl[AclCapacity]{};
};
struct NamedValue
{
    bool Used{false};
    UInt8 KeyIndex{0U};
    UInt8 NameLength{0U};
    UInt8 Name[MaximumNameBytes]{};
    Value Data{};
};
struct Snapshot
{
    Key Keys[KeyCapacity]{};
    NamedValue Values[ValueCapacity]{};
};
enum class ChangeKind : UInt8
{
    KeyCreated,
    KeyDeleted,
    ValueWritten,
    ValueDeleted,
    AclChanged
};
struct Change
{
    Path KeyPath{};
    Name ValueName{};
    ChangeKind Kind{ChangeKind::KeyCreated};
};
enum class Status : UInt8
{
    Ok,
    Invalid,
    Closed,
    NotFound,
    Exists,
    Denied,
    Full,
    NotEmpty,
    Corrupt,
    IoError,
    Exhausted,
    Empty,
    RescanRequired
};
// Exact byte I/O. Flush must durably order all previous writes. Callbacks
// cannot reenter the database. Capacity never changes while an instance is open.
struct Storage
{
    void* Context{nullptr};
    UInt64 Capacity{0U};
    bool (*Read)(void*, UInt64, UInt8*, Size) noexcept {nullptr};
    bool (*Write)(void*, UInt64, const UInt8*, Size) noexcept {nullptr};
    bool (*Flush)(void*) noexcept {nullptr};
};

class FormatCodec final
{
public:
    [[nodiscard]] static bool ValidValueName(TextView name) noexcept;
    [[nodiscard]] static bool ValidValue(ValueType type, const UInt8* bytes, Size length) noexcept;
    [[nodiscard]] static bool ValidAcl(const AclEntry* entries, Size count) noexcept;
    [[nodiscard]] static bool Encode(const Snapshot& state, UInt64 generation, UInt8 slot, UInt8* bytes,
                                     Size length) noexcept;
    // Failure leaves state and generation unchanged. bytes must be a full slot.
    [[nodiscard]] static bool Decode(const UInt8* bytes, Size length, UInt8 slot, Snapshot& state,
                                     UInt64& generation) noexcept;
    [[nodiscard]] static bool Validate(const UInt8* bytes, Size length, UInt8 slot, UInt64& generation) noexcept;
};

class Database final
{
public:
    Database() noexcept = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    [[nodiscard]] Status Format(Storage storage, const AclEntry* rootAcl, Size count) noexcept;
    [[nodiscard]] Status Open(Storage storage) noexcept;
    void Close() noexcept;
    [[nodiscard]] Status Create(Access access, TextView path) noexcept;
    [[nodiscard]] Status Read(Access access, TextView path, TextView name, Value& value) const noexcept;
    [[nodiscard]] Status Write(Access access, TextView path, TextView name, const Value& value) noexcept;
    [[nodiscard]] Status DeleteValue(Access access, TextView path, TextView name) noexcept;
    [[nodiscard]] Status EnumerateValue(Access access, TextView path, Size ordinal, Name& name,
                                        ValueType& type) const noexcept;
    [[nodiscard]] Status EnumerateSubkey(Access access, TextView path, Size ordinal, Name& name) const noexcept;
    [[nodiscard]] Status Delete(Access access, TextView path) noexcept;
    [[nodiscard]] Status GetAcl(Access access, TextView path, AclEntry* entries, Size capacity,
                                Size& count) const noexcept;
    [[nodiscard]] Status SetAcl(Access access, TextView path, const AclEntry* entries, Size count) noexcept;
    [[nodiscard]] Status Watch(Access access, TextView path, bool subtree, UInt64& handle) noexcept;
    [[nodiscard]] Status Poll(UInt64 handle, Change& change) noexcept;
    [[nodiscard]] Status Unwatch(UInt64 handle) noexcept;
    [[nodiscard]] UInt64 Generation() const noexcept
    {
        return m_open ? m_generation : 0U;
    }
    // Pure evaluation; invalid key/principal view yields zero. No identity bypass.
    [[nodiscard]] static UInt8 EffectiveRights(const Snapshot& state, Size key, Access access) noexcept;

private:
    struct Watcher
    {
        bool Used{false};
        bool Subtree{false};
        bool Overflow{false};
        bool DeleteAllowed{false};
        UInt64 Handle{0U};
        Access Caller{};
        Path Root{};
        Change Queue[QueueCapacity]{};
        Size First{0U};
        Size Count{0U};
    };
    [[nodiscard]] static bool ValidStorage(Storage storage) noexcept;
    [[nodiscard]] static Status Find(const Snapshot& state, TextView path, Size& key) noexcept;
    [[nodiscard]] static Size FindValue(const Snapshot& state, Size key, TextView name) noexcept;
    [[nodiscard]] bool Traversable(Size key, Access access) const noexcept;
    [[nodiscard]] Status Lookup(Access access, TextView path, UInt8 right, Size& key) const noexcept;
    [[nodiscard]] Status Commit(TextView path, TextView name, ChangeKind kind) noexcept;
    void Notify(TextView path, TextView name, ChangeKind kind) noexcept;
    [[nodiscard]] bool WatchReadable(const Watcher& watcher, TextView path) const noexcept;
    Storage m_storage{};
    Snapshot m_state{};
    Snapshot m_candidate{};
    UInt8 m_wire[SlotBytes]{};
    Watcher m_watchers[WatchCapacity]{};
    UInt64 m_generation{0U};
    UInt64 m_nextHandle{1U};
    UInt8 m_slot{1U};
    bool m_open{false};
};
} // namespace Asteria::User::Settings
#endif
