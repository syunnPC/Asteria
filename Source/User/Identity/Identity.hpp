// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_IDENTITY_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_IDENTITY_HPP_INCLUDED
#include "User/Crypto/Crypto.hpp"

namespace Asteria::User::Identity
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using PrincipalId = UInt64;
// Single sources for bounds and persisted offsets.
inline constexpr PrincipalId System{1U};
inline constexpr PrincipalId Administrator{2U};
inline constexpr PrincipalId Everyone{3U};
inline constexpr PrincipalId Users{4U};
inline constexpr PrincipalId Administrators{5U};
inline constexpr PrincipalId FirstCustomId{256U};
inline constexpr UInt64 MaximumCounter{~UInt64{0U}};
inline constexpr Size AccountCapacity{64U};
inline constexpr Size GroupCapacity{64U};
inline constexpr Size MembershipsPerAccount{12U};
inline constexpr Size MembershipCapacity{AccountCapacity * MembershipsPerAccount};
inline constexpr Size SubjectCapacity{16U};
inline constexpr Size MaximumNameBytes{63U};
inline constexpr Size MaximumPasswordBytes{1024U};
inline constexpr Size HeaderBytes{64U};
inline constexpr Size AccountRecordBytes{160U};
inline constexpr Size GroupRecordBytes{96U};
inline constexpr Size MembershipRecordBytes{16U};
inline constexpr Size GroupTableOffset{HeaderBytes + AccountCapacity * AccountRecordBytes};
inline constexpr Size MembershipTableOffset{GroupTableOffset + GroupCapacity * GroupRecordBytes};
inline constexpr Size BodyBytes{MembershipTableOffset + MembershipCapacity * MembershipRecordBytes};
inline constexpr Size SlotBytes{32768U};
inline constexpr Size SealBytes{32U};
inline constexpr Size SealOffset{SlotBytes - SealBytes};
inline constexpr Size StorageBytes{2U * SlotBytes};
inline constexpr Size ArenaBlocks{Crypto::LoginArgon2Defaults.MemoryKiB};
inline constexpr Size ArenaBytes{ArenaBlocks * sizeof(Crypto::Argon2Block)};
static_assert(GroupTableOffset == 10304U && MembershipTableOffset == 16448U && BodyBytes == 28736U);
static_assert(BodyBytes < SealOffset && StorageBytes == 65536U && ArenaBytes == 64U * 1024U * 1024U);
static_assert(1U + 2U + MembershipsPerAccount + 1U == SubjectCapacity);
static_assert(Crypto::LoginArgon2SaltBytes == 16U && Crypto::LoginArgon2TagBytes == 32U);
struct TextView
{
    const UInt8* Bytes{nullptr};
    Size Length{0U};
};
struct Name
{
    UInt8 Bytes[MaximumNameBytes]{};
    UInt8 Length{0U};
};
enum class Role : UInt8
{
    Standard,
    Admin
};
enum class Status : UInt8
{
    Ok,
    Invalid,
    Closed,
    NotFound,
    Exists,
    Full,
    Corrupt,
    IoError,
    Exhausted,
    NotProvisioned,
    Failed,
    RetryAt,
    Busy,
    Unavailable,
    Cancelled
};
struct AccountInfo
{
    PrincipalId Id{0U};
    UInt64 Revision{0U};
    Role AccountRole{Role::Standard};
    bool Disabled{true};
    Name Login{};
};
struct Group
{
    PrincipalId Id{0U};
    UInt64 Revision{0U};
    Name Label{};
};
// Credential-bearing codec/storage types are never account-management responses.
struct Credential
{
    UInt8 Profile{0U};
    UInt8 Salt[Crypto::LoginArgon2SaltBytes]{};
    UInt8 Tag[Crypto::LoginArgon2TagBytes]{};
};
struct Account
{
    AccountInfo Info{};
    Credential Password{};
};
struct Membership
{
    PrincipalId AccountId{0U};
    PrincipalId GroupId{0U};
};
struct Snapshot
{
    PrincipalId NextId{FirstCustomId};
    Account Accounts[AccountCapacity]{};
    Group Groups[GroupCapacity]{};
    Membership Memberships[MembershipCapacity]{};
};
struct Subjects
{
    PrincipalId Ids[SubjectCapacity]{};
    UInt8 Count{0U};
};
struct PrincipalContext
{
    PrincipalId AccountId{0U};
    UInt64 Revision{0U};
    UInt64 SessionId{0U};
    UInt64 OperationId{0U};
    Subjects AllowSubjects{};
    Subjects DenySubjects{};
};
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
    [[nodiscard]] static bool ValidName(TextView name) noexcept;
    [[nodiscard]] static bool ValidSnapshot(const Snapshot& state) noexcept;
    [[nodiscard]] static bool Encode(const Snapshot& state, UInt64 generation, UInt8 slot, UInt8* bytes,
                                     Size length) noexcept;
    // Disjoint buffers required. Failure preserves every output.
    [[nodiscard]] static bool Validate(const UInt8* bytes, Size length, UInt8 slot, UInt64& generation) noexcept;
    [[nodiscard]] static bool Decode(const UInt8* bytes, Size length, UInt8 slot, Snapshot& state,
                                     UInt64& generation) noexcept;
};
class Authentication;
class AccountStore final
{
public:
    AccountStore() noexcept = default;
    ~AccountStore() noexcept
    {
        Close();
    }
    AccountStore(const AccountStore&) = delete;
    AccountStore& operator=(const AccountStore&) = delete;
    [[nodiscard]] Status Format(Storage storage) noexcept;
    [[nodiscard]] Status Open(Storage storage) noexcept;
    void Close() noexcept;
    [[nodiscard]] UInt64 Generation() const noexcept
    {
        return m_open ? m_generation : 0U;
    }
    [[nodiscard]] Status Ready() const noexcept;
    [[nodiscard]] Status GetAccount(PrincipalId id, AccountInfo& out) const noexcept;
    [[nodiscard]] Status FindAccount(TextView name, AccountInfo& out) const noexcept;
    [[nodiscard]] Status GetGroup(PrincipalId id, Group& out) const noexcept;
    [[nodiscard]] Status CreateAccount(TextView name, Role role, PrincipalId& id) noexcept;
    [[nodiscard]] Status UpdateAccount(PrincipalId id, TextView name, Role role, bool disabled) noexcept;
    [[nodiscard]] Status DeleteAccount(PrincipalId id) noexcept;
    [[nodiscard]] Status CreateGroup(TextView name, PrincipalId& id) noexcept;
    [[nodiscard]] Status RenameGroup(PrincipalId id, TextView name) noexcept;
    [[nodiscard]] Status DeleteGroup(PrincipalId id) noexcept;
    [[nodiscard]] Status SetMemberships(PrincipalId id, const PrincipalId* groups, Size count) noexcept;
    [[nodiscard]] Status GetMemberships(PrincipalId id, PrincipalId* groups, Size capacity, Size& count) const noexcept;
    [[nodiscard]] Status BuildContext(PrincipalId id, UInt64 session, UInt64 operation,
                                      PrincipalContext& out) const noexcept;

private:
    friend class Authentication;
    [[nodiscard]] Status SetCredential(PrincipalId id, const Credential& credential) noexcept;
    [[nodiscard]] Status Commit() noexcept;
    [[nodiscard]] Size AccountIndex(PrincipalId id) const noexcept;
    [[nodiscard]] Size GroupIndex(PrincipalId id) const noexcept;
    Storage m_storage{};
    Snapshot m_state{};
    Snapshot m_candidate{};
    UInt8 m_wire[SlotBytes]{};
    UInt64 m_generation{0U};
    UInt8 m_slot{1U};
    bool m_open{false};
};
// Exact monotonic nanoseconds avoid shortening the specified delay through whole-second timestamp
// truncation.
inline constexpr UInt64 TimeTicksPerSecond{1000000000U};
inline constexpr UInt64 QuietSeconds{15U * 60U};
inline constexpr UInt64 GlobalDelaySeconds{1U};
struct Clock
{
    void* Context{nullptr};
    bool (*Read)(void*, UInt64&) noexcept {nullptr};
};
struct Bucket
{
    PrincipalId Id{0U};
    UInt64 Failures{0U};
    UInt64 LastFailure{0U};
    UInt64 Deadline{0U};
};
struct LimiterState
{
    Bucket Accounts[AccountCapacity]{};
    Bucket Unknown{};
    UInt64 GlobalDeadline{0U};
    UInt64 LastTime{0U};
    UInt64 NextSerial{1U};
    UInt64 Serial{0U};
    PrincipalId InFlightAccount{0U};
    bool InFlight{false};
    bool ClockSeen{false};
    bool Exhausted{false};
};
class AttemptLimiter final
{
public:
    AttemptLimiter(LimiterState& state, Clock clock) noexcept : m_state{state}, m_clock{clock}
    {
    }
    [[nodiscard]] Status Begin(PrincipalId account, UInt64& serial, UInt64& retryAt) noexcept;
    [[nodiscard]] Status Complete(UInt64 serial, bool success, bool cancelled = false) noexcept;
    [[nodiscard]] Status Recover() noexcept;

private:
    friend class Authentication;
    [[nodiscard]] bool Now(UInt64& time) noexcept;
    [[nodiscard]] Bucket* Find(PrincipalId account, bool create) noexcept;
    LimiterState& m_state;
    Clock m_clock;
};
struct Entropy
{
    void* Context{nullptr};
    bool (*Fill)(void*, UInt8*, Size) noexcept {nullptr};
};
struct Cancellation
{
    void* Context{nullptr};
    bool (*Requested)(void*) noexcept {nullptr};
    // Optional bounded service drain. Hash completion/cleanup remains mandatory.
    void (*Poll)(void*) noexcept {nullptr};
};
struct AuthenticationWorkspace
{
    Crypto::Workspace CryptoWork{};
    UInt8 Password[MaximumPasswordBytes]{};
    UInt8 Tag[Crypto::LoginArgon2TagBytes]{};
    Credential Stored{};
    AccountInfo Info{};
    PrincipalContext Context{};
};
static_assert(sizeof(AuthenticationWorkspace) <= 20U * 1024U);
class Authentication final
{
public:
    Authentication(AuthenticationWorkspace& work, Crypto::Argon2Block* arena, Size blocks) noexcept
        : m_work{work}, m_arena{arena}, m_blocks{blocks}
    {
    }
    [[nodiscard]] static bool ValidPassword(TextView password) noexcept;
    [[nodiscard]] Status Enroll(TextView password, Entropy entropy, Credential& out, Cancellation cancel = {}) noexcept;
    [[nodiscard]] Status Enroll(AccountStore& store, PrincipalId id, TextView password, Entropy entropy,
                                Cancellation cancel = {}) noexcept;
    [[nodiscard]] Status Verify(AccountStore& store, AttemptLimiter& limiter, TextView name, TextView password,
                                UInt64 session, UInt64 operation, PrincipalContext& out, UInt64& retryAt,
                                Cancellation cancel = {}) noexcept;

private:
    [[nodiscard]] bool Buffers(TextView password, const void* output, Size outputBytes) const noexcept;
    [[nodiscard]] Status Hash(Size length, Cancellation cancel) noexcept;
    void Clear() noexcept;
    AuthenticationWorkspace& m_work;
    Crypto::Argon2Block* m_arena;
    Size m_blocks;
};
} // Namespace Asteria::User::Identity
#endif
