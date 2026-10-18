// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/AuthService.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/ObjectRequests.hpp"
namespace
{
struct AuthPlacement
{
    void* Address;
};
} // Namespace
[[nodiscard]] inline void* operator new(Asteria::Foundation::Size, AuthPlacement place) noexcept
{
    return place.Address;
}
namespace Asteria::User::Identity
{
namespace
{
using System::CapabilityHandle;
class AuthService final
{
public:
    [[nodiscard]] bool Start(const System::SystemCallClient&, const UInt64*, SeatClock, UInt64) noexcept;
    [[nodiscard]] bool Step() noexcept;
    void AdmissionFailure() noexcept
    {
        Words();
        m_words[0U] = m_tag;
        m_words[1U] = static_cast<UInt64>(Status::Unavailable);
        m_words[2U] = m_admissionStep;
        m_words[3U] = m_admissionError;
        static_cast<void>(Send(m_root, SeatFace::AuthControl, SeatMethod::Reply, 1U));
    }
    void Clear() noexcept
    {
        Crypto::Wipe({m_password, sizeof(m_password)});
        Crypto::Wipe({m_name, sizeof(m_name)});
        Crypto::Wipe({reinterpret_cast<UInt8*>(&m_work), sizeof(m_work)});
        m_result = {};
        m_store.Close();
    }

private:
    static bool Read(void*, UInt64, UInt8*, Size) noexcept;
    static bool NoWrite(void*, UInt64, const UInt8*, Size) noexcept
    {
        return false;
    }
    static bool NoFlush(void*) noexcept
    {
        return false;
    }
    static bool Time(void*, UInt64&) noexcept;
    static bool Cancelled(void*) noexcept;
    static void Poll(void*) noexcept;
    [[nodiscard]] bool Map(CapabilityHandle, CapabilityHandle, UInt64, UInt64, UInt64, UInt8 = 0x0BU,
                           UInt64 = 0U) noexcept;
    [[nodiscard]] bool Memory(UInt64, CapabilityHandle&, UInt8 = 0x0BU) noexcept;
    [[nodiscard]] bool Receive(CapabilityHandle, bool) noexcept;
    [[nodiscard]] bool Control() noexcept;
    [[nodiscard]] bool Submit() noexcept;
    [[nodiscard]] bool Send(CapabilityHandle, SeatFace, SeatMethod, UInt64, CapabilityHandle = {},
                            bool = false) noexcept;
    [[nodiscard]] bool Answers() noexcept;
    void Words() noexcept
    {
        for (auto& w : m_words)
        {
            w = 0U;
        }
    }
    void Close(CapabilityHandle h) noexcept
    {
        if (h.Selector != 0U)
        {
            static_cast<void>(m_client->CloseCapability(h));
        }
    }
    void Verify() noexcept;
    [[nodiscard]] bool Cleanup() noexcept;
    const System::SystemCallClient* m_client{};
    SeatClock m_clock{};
    CapabilityHandle m_domain{};
    CapabilityHandle m_space{};
    CapabilityHandle m_wait{};
    CapabilityHandle m_control{};
    CapabilityHandle m_submit{};
    CapabilityHandle m_root{};
    CapabilityHandle m_feedback{};
    CapabilityHandle m_snapshotMemory{};
    CapabilityHandle m_memories[AuthMemoryObjects]{};
    CapabilityHandle m_frames[AuthRuns]{};
    // Two control replies plus a reserved terminal-result slot. Frames are
    // frozen until delivery; borrowed reply Endpoints close only afterwards.
    static constexpr Size ReplyCapacity{3U};
    CapabilityHandle m_pendingTarget[ReplyCapacity]{};
    Size m_pendingBytes[ReplyCapacity]{};
    Size m_pendingFirst{};
    Size m_pendingCount{};
    bool m_pendingClose[ReplyCapacity]{};
    bool m_pendingOutcome[ReplyCapacity]{};
    UInt64 m_pendingEpoch[ReplyCapacity]{};
    UInt64 m_pendingTransaction[ReplyCapacity]{};
    UInt64 m_tag{};
    UInt64 m_scale{};
    UInt64 m_base{};
    UInt64 m_rootSender{};
    UInt64 m_loginSender{};
    UInt64 m_next{};
    UInt64 m_transaction{};
    UInt64 m_epoch{};
    UInt64 m_attempt{};
    UInt64 m_session{};
    UInt64 m_expected{};
    UInt64 m_started{};
    UInt64 m_admissionStep{};
    UInt64 m_admissionError{};
    Purpose m_purpose{};
    bool m_cancelled{false};
    bool m_hash{false};
    bool m_failed{false};
    const UInt8* m_fixture{};
    LimiterState* m_limiter{};
    AccountStore m_store{};
    AuthenticationWorkspace m_work{};
    PrincipalContext m_result{};
    UInt8 m_password[MaximumPasswordBytes]{};
    UInt8 m_name[64U]{};
    Size m_passwordBytes{};
    Size m_nameBytes{};
    UInt64 m_words[SeatMaximumWords]{};
    SeatMessage m_message{};
    alignas(8) UInt8 m_frame[SeatFrameBytes]{}, m_pending[ReplyCapacity][SeatFrameBytes]{},
        m_delivery[SeatDeliveryBytes]{};
};
alignas(AuthService) UInt8 g_authStorage[sizeof(AuthService)]{};
AuthService* g_auth{};
bool AuthService::Memory(UInt64 pages, CapabilityHandle& out, UInt8 permissions) noexcept
{
    alignas(8) UInt8 request[Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    System::StoreWord(request + Abi::ObjectCreatePagesOffset, pages);
    request[Abi::ObjectCreatePermissionsOffset] = permissions;
    request[Abi::ObjectCreateCachePolicyOffset] = Abi::ObjectCreateCacheWriteBack;
    const auto created =
        m_client->CreateObject(m_domain, Abi::ObjectTypeMemoryObject, System::AddressOf(request), sizeof(request));
    out = {created.Value0, created.Value1};
    m_admissionError = static_cast<UInt64>(created.Error);
    return created.Error == Abi::SystemCallError::None;
}
bool AuthService::Map(CapabilityHandle memory, CapabilityHandle frame, UInt64 address, UInt64 pages, UInt64 offset,
                      UInt8 permissions, UInt64 frameOffset) noexcept
{
    alignas(8) UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, m_space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    System::StoreWord(request + Abi::FaultResolveFramePageOffset, frameOffset);
    request[Abi::FaultResolvePermissionsOffset] = permissions;
    const auto result = m_client->MapMemory(System::AddressOf(request));
    m_admissionError = static_cast<UInt64>(result.Error);
    return result.Error == Abi::SystemCallError::None;
}
bool AuthService::Read(void* p, UInt64 offset, UInt8* out, Size bytes) noexcept
{
    auto& self = *static_cast<AuthService*>(p);
    if (offset > StorageBytes || bytes > StorageBytes - offset)
    {
        return false;
    }
    for (Size i{0U}; i < bytes; ++i)
    {
        out[i] = self.m_fixture[offset + i];
    }
    return true;
}
bool AuthService::Time(void* p, UInt64& out) noexcept
{
    const auto& self = *static_cast<AuthService*>(p);
    if (self.m_clock == nullptr || self.m_scale == 0U)
    {
        return false;
    }
    const UInt64 us = self.m_clock() / self.m_scale;
    if (us > MaximumCounter / 1000U)
    {
        return false;
    }
    out = us * 1000U;
    return true;
}
bool AuthService::Cancelled(void* p) noexcept
{
    return static_cast<AuthService*>(p)->m_cancelled;
}
void AuthService::Poll(void* p) noexcept
{
    auto& self = *static_cast<AuthService*>(p);
    // RFC 9106 section 3.4 public segment boundary: one bounded control
    // delivery and pending answer; never reenter verification or accept Submit.
    if (!self.Answers() || !self.Receive(self.m_control, true))
    {
        self.m_failed = true;
        self.m_cancelled = true;
    }
}
bool AuthService::Send(CapabilityHandle target, SeatFace face, SeatMethod method, UInt64 transaction,
                       CapabilityHandle attachment, bool closeTarget) noexcept
{
    if (m_pendingCount == ReplyCapacity)
    {
        return false;
    }
    const auto bytes = WriteSeatFrame(m_frame, face, method, transaction, m_words, attachment);
    if (bytes == 0U || target.Selector == 0U)
    {
        return false;
    }
    const auto sent = m_pendingCount != 0U ? Abi::SystemCallError::WouldBlock
                                           : m_client->Send(target, System::AddressOf(m_frame), bytes).Error;
    if (sent == Abi::SystemCallError::WouldBlock)
    {
        const Size at = (m_pendingFirst + m_pendingCount) % ReplyCapacity;
        for (Size i{0U}; i < bytes; ++i)
        {
            m_pending[at][i] = m_frame[i];
        }
        m_pendingBytes[at] = bytes;
        m_pendingTarget[at] = target;
        m_pendingClose[at] = closeTarget;
        m_pendingOutcome[at] = method == SeatMethod::Result && m_words[4U] != static_cast<UInt64>(Status::Cancelled);
        m_pendingEpoch[at] = m_words[0U];
        m_pendingTransaction[at] = m_words[1U];
        ++m_pendingCount;
        return true;
    }
    if (closeTarget)
    {
        Close(target);
    }
    return sent == Abi::SystemCallError::None;
}
bool AuthService::Answers() noexcept
{
    if (m_pendingCount == 0U)
    {
        return true;
    }
    const Size at = m_pendingFirst;
    // A frozen outcome cannot outlive cancellation/replacement. Cancelled carries only cleanup
    // evidence, never a principal, and may still reach root after the screen has retired its
    // transaction.
    const bool retired =
        m_pendingOutcome[at] && (m_pendingEpoch[at] != m_epoch || m_pendingTransaction[at] != m_transaction);
    const auto sent =
        retired ? Abi::SystemCallError::None
                : m_client->Send(m_pendingTarget[at], System::AddressOf(m_pending[at]), m_pendingBytes[at]).Error;
    if (sent == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (m_pendingClose[at])
    {
        Close(m_pendingTarget[at]);
    }
    m_pendingBytes[at] = 0U;
    m_pendingTarget[at] = {};
    m_pendingClose[at] = false;
    m_pendingOutcome[at] = false;
    m_pendingEpoch[at] = 0U;
    m_pendingTransaction[at] = 0U;
    m_pendingFirst = (at + 1U) % ReplyCapacity;
    --m_pendingCount;
    return sent == Abi::SystemCallError::None;
}
[[gnu::noinline]] bool AuthService::Start(const System::SystemCallClient& client, const UInt64* block, SeatClock clock,
                                          UInt64 region) noexcept
{
    m_client = &client;
    m_clock = clock;
    m_base = region;
    m_tag = block[Abi::ComponentIncarnationWord];
    m_scale = block[Abi::ComponentClockTicksPerMicrosecondWord];
    m_domain = {block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]};
    m_space = {block[Abi::ComponentAccountSpaceWord], block[Abi::ComponentAccountSpaceWord + 1U]};
    m_wait = {block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]};
    m_control = {block[Abi::ComponentReceiveWord], block[Abi::ComponentReceiveWord + 1U]};
    m_root = {block[Abi::ComponentReportWord], block[Abi::ComponentReportWord + 1U]};
    if (m_tag == 0U || m_tag > 0xFFFF'FFFFU || m_scale == 0U)
    {
        return false;
    }
    m_next = m_tag << 32U;
    for (Size memory{0U}; memory < AuthMemoryObjects; ++memory)
    {
        m_admissionStep = 0x100U + memory;
        if (!Memory(AuthMemoryPages, m_memories[memory]))
        {
            return false;
        }
        for (Size part{0U}; part < AuthRuns / AuthMemoryObjects; ++part)
        {
            const Size at = memory * (AuthRuns / AuthMemoryObjects) + part;
            m_admissionStep = 0x200U + at;
            const auto frame = System::CreateRamFrame(client, m_domain, AuthRunPages, 0U, 0U);
            m_admissionError = static_cast<UInt64>(frame.Error);
            if (frame.Error != Abi::SystemCallError::None)
            {
                return false;
            }
            m_frames[at] = {frame.Value0, frame.Value1};
            m_admissionStep = 0x300U + at;
            if (!Map(m_memories[memory], m_frames[at], region + AuthArenaAddress + at * AuthRunPages * SeatPageBytes,
                     AuthRunPages, part * AuthRunPages))
            {
                return false;
            }
        }
    }
    m_admissionStep = 0x400U;
    if (!Memory(1U, m_snapshotMemory, 0x09U))
    {
        return false;
    }
    CapabilityHandle fixtureMemory{};
    CapabilityHandle stateMemory{};
    m_admissionStep = 0x401U;
    if (!Memory(StorageBytes / SeatPageBytes, fixtureMemory, 0x09U) || !Memory(1U, stateMemory))
    {
        return false;
    }
    const CapabilityHandle fixture{block[45U], block[46U]};
    const CapabilityHandle state{block[50U], block[51U]};
    m_admissionStep = 0x402U;
    if (!Map(fixtureMemory, fixture, region + AuthFixtureAddress, StorageBytes / SeatPageBytes, 0U, 0x09U,
             block[49U]) ||
        !Map(stateMemory, state, region + AuthFixtureAddress + StorageBytes, 1U, 0U))
    {
        return false;
    }
    m_fixture = reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(region + AuthFixtureAddress));
    m_limiter = reinterpret_cast<LimiterState*>(
        static_cast<Foundation::UIntPointer>(region + AuthFixtureAddress + StorageBytes));
    m_admissionStep = 0x403U;
    AttemptLimiter limiter{*m_limiter, {this, Time}};
    if (limiter.Recover() != Status::Ok)
    {
        return false;
    }
    m_admissionStep = 0x404U;
    if (m_store.Open({this, StorageBytes, Read, NoWrite, NoFlush}) != Status::Ok || m_store.Ready() != Status::Ok)
    {
        return false;
    }
    m_admissionStep = 0x405U;
    alignas(8) UInt8 request[Abi::ObjectCreateEndpointRequestSizeBytes]{};
    System::BuildDefaultEndpointRequest(request);
    System::StoreWord(request + Abi::ObjectCreateEndpointSealedSelectorOffset, AuthSubmitSelector);
    const auto endpoint =
        client.CreateObject(m_domain, Abi::ObjectTypeEndpoint, System::AddressOf(request), sizeof(request));
    m_admissionError = static_cast<UInt64>(endpoint.Error);
    if (endpoint.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_submit = {endpoint.Value0, endpoint.Value1};
    if (client.BindWaitSet(m_wait, m_submit, AuthSubmitSelector).Error != Abi::SystemCallError::None)
    {
        return false;
    }
    Words();
    m_words[0U] = m_tag;
    m_words[1U] = AuthArenaPages;
    m_words[2U] = AuthRuns;
    m_words[3U] = 1U;
    return Send(m_root, SeatFace::AuthControl, SeatMethod::Ready, ++m_next);
}
bool AuthService::Receive(CapabilityHandle endpoint, bool control) noexcept
{
    const auto received = m_client->Receive(endpoint, System::AddressOf(m_delivery), sizeof(m_delivery));
    if (received.Error == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (received.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    const auto bytes = static_cast<Size>(received.Value1);
    if (!ReadSeatFrame(m_delivery, bytes, m_message))
    {
        // Bounded canonical attachment disposal even for refused payloads.
        DisposeSeatAttachments(*m_client, m_delivery, bytes);
        return true;
    }
    return control ? Control() : Submit();
}
[[gnu::noinline]] bool AuthService::Control() noexcept
{
    if (m_message.Face != SeatFace::AuthControl || m_message.Selector != AuthControlSelector ||
        (m_message.Rights & Component::EchoManageRight) == 0U ||
        (m_rootSender != 0U && m_message.Sender != m_rootSender))
    {
        Close(m_message.Attachment);
        return true;
    }
    if (m_message.Sender == 0U)
    {
        Close(m_message.Attachment);
        return true;
    }
    if (m_rootSender == 0U)
    {
        m_rootSender = m_message.Sender;
    }
    const auto* w = m_message.Words;
    switch (m_message.Method)
    {
    case SeatMethod::Query:
        Words();
        m_words[0U] = m_tag;
        m_words[1U] = static_cast<UInt64>(Status::Ok);
        if (m_pendingCount >= ReplyCapacity - 1U)
        {
            Close(m_message.Attachment);
            return true;
        }
        return Send(m_message.Attachment, SeatFace::AuthControl, SeatMethod::Reply, m_message.Transaction, {}, true);
    case SeatMethod::Configure:
        if (m_hash || m_transaction != 0U)
        {
            Close(m_message.Attachment);
            return true;
        }
        Close(m_feedback);
        m_feedback = m_message.Attachment;
        m_loginSender = w[1U];
        Words();
        m_words[0U] = m_tag;
        return Send(m_root, SeatFace::AuthControl, SeatMethod::OpenSubmit, m_message.Transaction, m_submit);
    case SeatMethod::OpenSubmit:
        Close(m_message.Attachment);
        return true; // Response is handled by root's installed report face below.
    case SeatMethod::Begin:
        if (m_hash || m_feedback.Selector == 0U || w[1U] != m_loginSender || m_next == MaximumCounter)
        {
            return true;
        }
        if (w[0U] < m_epoch)
        {
            return true;
        }
        if (w[0U] != m_epoch || m_transaction == 0U)
        {
            m_epoch = w[0U];
            m_transaction = ++m_next;
            m_attempt = 0U;
            m_purpose = static_cast<Purpose>(w[2U]);
            m_expected = w[3U];
            m_session = w[4U];
            m_cancelled = false;
        }
        Words();
        m_words[0U] = m_epoch;
        m_words[1U] = static_cast<UInt64>(Status::Ok);
        m_words[2U] = m_transaction;
        return Send(m_root, SeatFace::AuthControl, SeatMethod::Reply, m_message.Transaction);
    case SeatMethod::Cancel:
        if (w[0U] == m_epoch && w[1U] == m_transaction)
        {
            m_cancelled = true;
            if (!m_hash)
            {
                m_transaction = 0U;
                Crypto::Wipe({m_password, sizeof(m_password)});
            }
        }
        return true;
    case SeatMethod::CheckRevision:
    {
        AccountInfo info{};
        const bool valid = m_store.GetAccount(w[0U], info) == Status::Ok && !info.Disabled && info.Revision == w[1U];
        Words();
        m_words[0U] = m_tag;
        m_words[1U] = static_cast<UInt64>(valid ? Status::Ok : Status::Failed);
        return Send(m_root, SeatFace::AuthControl, SeatMethod::Reply, m_message.Transaction);
    }
    default:
        Close(m_message.Attachment);
        return true;
    }
}
[[gnu::noinline]] bool AuthService::Submit() noexcept
{
    const auto* w = m_message.Words;
    if (m_message.Face != SeatFace::AuthSubmit || m_message.Selector != AuthSubmitSelector ||
        m_message.Sender != m_loginSender || w[0U] != m_epoch || w[1U] != m_transaction || m_transaction == 0U ||
        m_hash || m_pendingCount != 0U)
    {
        Close(m_message.Attachment);
        return true;
    }
    if (m_message.Method == SeatMethod::Cancel)
    {
        m_cancelled = true;
        m_transaction = 0U;
        return true;
    }
    if (m_message.Method != SeatMethod::Submit || w[2U] <= m_attempt)
    {
        Close(m_message.Attachment);
        return true;
    }
    m_nameBytes = static_cast<Size>(w[3U]);
    m_passwordBytes = static_cast<Size>(w[4U]);
    for (Size i{0U}; i < m_nameBytes; ++i)
    {
        m_name[i] = static_cast<UInt8>(w[5U + i / 8U] >> (8U * (i % 8U)));
    }
    if (!FormatCodec::ValidName({m_name, m_nameBytes}))
    {
        Close(m_message.Attachment);
        return true;
    }
    if (!Map(m_snapshotMemory, m_message.Attachment, m_base + AuthPasswordAddress, 1U, 0U, 0x09U))
    {
        Close(m_message.Attachment);
        return true;
    }
    const auto* input =
        reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(m_base + AuthPasswordAddress));
    for (Size i{0U}; i < m_passwordBytes; ++i)
    {
        m_password[i] = input[i];
    }
    const bool unmapped =
        m_client->UnmapMemory(m_space, m_base + AuthPasswordAddress, 1U).Error == Abi::SystemCallError::None;
    Close(m_message.Attachment);
    if (!unmapped)
    {
        Crypto::Wipe({m_password, sizeof(m_password)});
        return false;
    }
    m_attempt = w[2U];
    m_hash = true;
    m_cancelled = false;
    Words();
    m_words[0U] = m_epoch;
    m_words[1U] = m_transaction;
    m_words[2U] = m_attempt;
    m_words[3U] = static_cast<UInt64>(Status::Ok);
    return Send(m_feedback, SeatFace::AuthSubmit, SeatMethod::Copied, m_message.Transaction);
}
[[gnu::noinline]] void AuthService::Verify() noexcept
{
    UInt64 retry{0U};
    static_cast<void>(Time(this, m_started));
    AttemptLimiter limiter{*m_limiter, {this, Time}};
    Authentication auth{
        m_work, reinterpret_cast<Crypto::Argon2Block*>(static_cast<Foundation::UIntPointer>(m_base + AuthArenaAddress)),
        ArenaBlocks};
    // Unlock is pinned to the account, independently of the editable label.
    AccountInfo account{};
    const bool fixed = m_purpose != Purpose::Unlock || m_store.GetAccount(m_expected, account) == Status::Ok;
    if (m_purpose == Purpose::Unlock && fixed)
    {
        m_nameBytes = account.Login.Length;
        for (Size i{0U}; i < m_nameBytes; ++i)
        {
            m_name[i] = account.Login.Bytes[i];
        }
    }
    else
    {
        static_cast<void>(m_store.FindAccount({m_name, m_nameBytes}, account));
    }
    const auto status = auth.Verify(m_store, limiter, {m_name, m_nameBytes}, {m_password, m_passwordBytes}, m_session,
                                    0U, m_result, retry, {this, Cancelled, Poll});
    UInt64 completed{0U};
    static_cast<void>(Time(this, completed));
    Crypto::Wipe({m_password, sizeof(m_password)});
    m_hash = false;
    System::OrderDmaStorage(); // Publish accepted/terminal nonsecret checkpoint before Result.
    const UInt64 elapsed = completed >= m_started ? completed - m_started : 0U;
    const bool clean = Cleanup();
    if (!clean)
    {
        m_failed = true;
    }
    if (m_failed)
    {
        m_transaction = 0U;
        m_result = {};
        return;
    }
    Words();
    m_words[0U] = m_epoch;
    m_words[1U] = m_transaction;
    m_words[2U] = m_attempt;
    m_words[3U] = static_cast<UInt64>(m_purpose);
    m_words[4U] = static_cast<UInt64>(fixed ? status : Status::Failed);
    const bool success = status == Status::Ok && fixed;
    m_words[5U] = success ? m_result.AccountId : 0U;
    m_words[6U] = success ? m_result.Revision : 0U;
    // Failed completed verification returns its account/global deadline too.
    m_words[7U] = retry;
    if (!success && status == Status::Failed)
    {
        for (const auto& bucket : m_limiter->Accounts)
        {
            if (bucket.Id == account.Id && bucket.Deadline > m_words[7U])
            {
                m_words[7U] = bucket.Deadline;
            }
        }
        if (account.Id == 0U && m_limiter->Unknown.Deadline > m_words[7U])
        {
            m_words[7U] = m_limiter->Unknown.Deadline;
        }
        if (m_limiter->GlobalDeadline > m_words[7U])
        {
            m_words[7U] = m_limiter->GlobalDeadline;
        }
    }
    m_words[8U] = elapsed;
    m_words[9U] = clean ? 1U : 0U;
    if (success)
    {
        m_words[10U] = m_result.DenySubjects.Count;
        for (Size i{0U}; i < m_result.DenySubjects.Count; ++i)
        {
            m_words[11U + i] = m_result.DenySubjects.Ids[i];
        }
    }
    if (!Send(m_root, SeatFace::AuthControl, SeatMethod::Result, m_transaction))
    {
        m_failed = true;
    }
    m_result = {};
    if (m_cancelled)
    {
        m_transaction = 0U;
    }
}
[[gnu::noinline]] bool AuthService::Cleanup() noexcept
{
    // Inspect only zero/nonzero, never format credential bytes.
    // The full profile and caller workspace have already been wiped.
    UInt64 residue{0U};
    const auto* arena =
        reinterpret_cast<const volatile UInt64*>(static_cast<Foundation::UIntPointer>(m_base + AuthArenaAddress));
    for (Size i{0U}; i < ArenaBytes / sizeof(UInt64); ++i)
    {
        residue |= arena[i];
    }
    const auto* work = reinterpret_cast<const volatile UInt8*>(&m_work);
    for (Size i{0U}; i < sizeof(m_work); ++i)
    {
        residue |= work[i];
    }
    for (Size i{0U}; i < sizeof(m_password); ++i)
    {
        residue |= m_password[i];
    }
    return residue == 0U;
}
[[gnu::noinline]] bool AuthService::Step() noexcept
{
    if (m_failed || !Answers())
    {
        return false;
    }
    if (!Receive(m_control, true))
    {
        return false;
    }
    if (m_pendingCount != 0U)
    {
        return true;
    }
    if (m_hash)
    {
        Verify();
        return !m_failed;
    }
    return Receive(m_submit, false);
}
} // Namespace
UInt64 ServeAuth(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                 SeatClock clock, UInt64 region) noexcept
{
    if (!InitializeAuth(client, block, clock, region))
    {
        ClearAuth();
        return 0xC0DE'0036U;
    }
    for (;;)
    {
        if (!AdvanceAuth())
        {
            ClearAuth();
            return 0xC0DE'00F6U;
        }
        const auto waited =
            client.Wait({block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]}, 1000U);
        if (waited.Error != Abi::SystemCallError::None && waited.Error != Abi::SystemCallError::TimedOut)
        {
            ClearAuth();
            return 0xC0DE'00F7U;
        }
    }
}
bool InitializeAuth(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                    SeatClock clock, UInt64 region) noexcept
{
    g_auth = new (AuthPlacement{g_authStorage}) AuthService{};
    const bool ready = g_auth->Start(client, block, clock, region);
    if (!ready)
    {
        g_auth->AdmissionFailure();
    }
    return ready;
}
bool AdvanceAuth() noexcept
{
    return g_auth != nullptr && g_auth->Step();
}
void ClearAuth() noexcept
{
    if (g_auth != nullptr)
    {
        g_auth->Clear();
    }
}
} // Namespace Asteria::User::Identity
