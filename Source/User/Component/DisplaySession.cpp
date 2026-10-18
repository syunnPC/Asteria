// SPDX-License-Identifier: Apache-2.0

#include "User/Component/DisplaySession.hpp"

#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::Component
{
namespace
{
constexpr Foundation::UInt64 FaceTag{0x4453'5001U};
constexpr Foundation::UInt64 ControlTag{0x4453'5002U};
constexpr Foundation::UInt64 TrustedTag{0x4453'5003U};
constexpr Foundation::UInt8 ReadUser{0x09U};
[[nodiscard, gnu::noinline]] Abi::SystemCallResponse CreateEndpoint(const System::SystemCallClient& client,
                                                                    const System::CapabilityHandle domain,
                                                                    const System::CapabilityHandle waitSet,
                                                                    const Foundation::UInt64 tag) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateEndpointRequestSizeBytes]{};
    System::BuildDefaultEndpointRequest(request);
    if (tag == TrustedTag)
    {
        System::StoreWord(request + Abi::ObjectCreateEndpointSealedSelectorOffset, Identity::TrustedDrawSelector);
    }
    const auto created =
        client.CreateObject(domain, Abi::ObjectTypeEndpoint, System::AddressOf(request), sizeof(request));
    if (created.Error != Abi::SystemCallError::None)
    {
        return created;
    }
    const System::CapabilityHandle handle{created.Value0, created.Value1};
    const auto bound = client.BindWaitSet(waitSet, handle, tag);
    if (bound.Error != Abi::SystemCallError::None)
    {
        static_cast<void>(client.DestroyObject(handle, Abi::ObjectTypeEndpoint));
        static_cast<void>(client.CloseCapability(handle));
        return bound;
    }
    return created;
}
[[nodiscard, gnu::noinline]] Abi::SystemCallResponse CreateReadMemory(const System::SystemCallClient& client,
                                                                      const System::CapabilityHandle domain,
                                                                      const Foundation::UInt64 pages) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    System::StoreWord(request + Abi::ObjectCreatePagesOffset, pages);
    request[Abi::ObjectCreatePermissionsOffset] = ReadUser;
    request[Abi::ObjectCreateCachePolicyOffset] = Abi::ObjectCreateCacheWriteBack;
    return client.CreateObject(domain, Abi::ObjectTypeMemoryObject, System::AddressOf(request), sizeof(request));
}
[[nodiscard, gnu::noinline]] Abi::SystemCallError
MapRead(const System::SystemCallClient& client, const System::CapabilityHandle space,
        const System::CapabilityHandle memory, const System::CapabilityHandle frame, const Foundation::UInt64 address,
        const Foundation::UInt64 pages, const Foundation::UInt64 offset) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] = ReadUser;
    return client.MapMemory(System::AddressOf(request)).Error;
}
} // namespace
void DisplaySession::Configure(const System::CapabilityHandle domain, const System::CapabilityHandle space,
                               const System::CapabilityHandle waitSet, const Foundation::UInt64 address,
                               const Graphics::PixelFormat format, DisplayInfo& info,
                               Graphics::Composition::PlaneStorage& planes,
                               Graphics::Composition::DamageList& damage) noexcept
{
    m_routing = false;
    m_pointerStale = false;
    m_staleCredit = false;
    m_dropped = 0U;
    m_failed = false;
    m_frozenCount = 0U;
    m_face = {};
    m_control = {};
    m_memory = {};
    m_present = {};
    m_credit = {};
    m_frameCount = 0U;
    m_mapped = 0U;
    m_first = 0U;
    m_count = 0U;
    m_serial = 0U;
    m_buffer = {};
    m_buffer.Format = format;
    for (auto& frame : m_frames)
    {
        frame = {};
    }
    m_domain = domain;
    m_space = space;
    m_waitSet = waitSet;
    m_address = address;
    m_info = &info;
    m_planes = &planes;
    m_damage = &damage;
    planes.Session().Configure({.X = 0, .Y = 0, .Width = info.Width, .Height = info.Height});
}
bool DisplaySession::Open(const System::SystemCallClient& client) noexcept
{
#if defined(ASTERIA_DISPLAY_CONSOLE_ONLY)
    return false;
#endif
    if (m_face.Selector != 0U)
    {
        return true;
    }
    const auto face = CreateEndpoint(client, m_domain, m_waitSet, FaceTag);
    if (face.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_face = {face.Value0, face.Value1};
    const auto control = CreateEndpoint(client, m_domain, m_waitSet, ControlTag);
    if (control.Error != Abi::SystemCallError::None)
    {
        static_cast<void>(client.DestroyObject(m_face, Abi::ObjectTypeEndpoint));
        static_cast<void>(client.CloseCapability(m_face));
        m_face = {};
        return false;
    }
    m_control = {control.Value0, control.Value1};
    return true;
}
bool DisplaySession::Reply(const System::SystemCallClient& client, const System::CapabilityHandle reply,
                           const Foundation::UInt64 transaction, const Foundation::UInt64 kind,
                           const bool opened) noexcept
{
    for (auto& word : m_words)
    {
        word = 0U;
    }
    m_words[0U] = transaction;
    m_words[1U] = kind;
    m_words[2U] = m_info->Version;
    m_words[3U] = m_info->Features;
    m_words[4U] = m_info->Width | (static_cast<Foundation::UInt64>(m_info->Height) << 32U);
    m_words[5U] = m_info->Columns | (static_cast<Foundation::UInt64>(m_info->Rows) << 32U);
    m_words[6U] = m_info->Epoch;
    m_words[7U] = m_info->PointerX | (static_cast<Foundation::UInt64>(m_info->PointerY) << 32U);
    m_words[8U] = m_info->PointerSequence;
    const System::CapabilityHandle handles[]{m_face, m_control};
    const auto size = Session::WriteSessionFaceFrame(m_frame, DisplayReplyMethod, transaction, m_words,
                                                     opened ? handles : nullptr, opened ? 2U : 0U);
    const auto error = client.Send(reply, System::AddressOf(m_frame), size).Error;
    if (error == Abi::SystemCallError::WouldBlock)
    {
        return false;
    }
    static_cast<void>(client.CloseCapability(reply));
    return true;
}
bool DisplaySession::Release(const System::SystemCallClient& client) noexcept
{
    // Never close a Frame while its mapping survives. Drain can then release the producer's domain.
    m_planes->Session().SetBuffer({});
    for (Foundation::Size i{0U}; i < m_frameCount; ++i)
    {
        if (m_frames[i].Selector != 0U)
        {
            if (client.UnmapMemory(m_space, m_address + m_offsets[i] * Session::SessionPageBytes, m_pages[i]).Error !=
                Abi::SystemCallError::None)
            {
                m_failed = true;
                return false;
            }
            static_cast<void>(client.CloseCapability(m_frames[i]));
            m_frames[i] = {};
        }
    }
    m_planes->Session().SetBuffer({});
    m_damage->Add(m_planes->Session().Bounds());
    if (m_memory.Selector != 0U)
    {
        if (client.DestroyObject(m_memory, Abi::ObjectTypeMemoryObject).Error != Abi::SystemCallError::None)
        {
            m_failed = true;
            return false;
        }
        static_cast<void>(client.CloseCapability(m_memory));
        m_memory = {};
    }
    m_frameCount = 0U;
    m_mapped = 0U;
    m_serial = 0U;
    return true;
}
bool DisplaySession::Add(const System::SystemCallClient& client, const Session::SessionFaceMessage& message,
                         const Foundation::Size first) noexcept
{
    if ((m_memory.Selector == 0U) || (first + message.AttachmentCount > m_frameCount))
    {
        return false;
    }
    for (Foundation::Size i{0U}; i < message.AttachmentCount; ++i)
    {
        if (m_frames[first + i].Selector != 0U)
        {
            return false;
        }
    }
    for (Foundation::Size i{0U}; i < message.AttachmentCount; ++i)
    {
        const Foundation::Size at = first + i;
        if (MapRead(client, m_space, m_memory, message.Attachments[i],
                    m_address + m_offsets[at] * Session::SessionPageBytes, m_pages[at],
                    m_offsets[at]) != Abi::SystemCallError::None)
        {
            // Earlier attachments in this batch now belong to this owner;
            // close only the unconsumed remainder here.
            for (Foundation::Size left{i}; left < message.AttachmentCount; ++left)
            {
                static_cast<void>(client.CloseCapability(message.Attachments[left]));
            }
            static_cast<void>(Release(client));
            return true;
        }
        m_frames[at] = message.Attachments[i];
        ++m_mapped;
    }
    if (m_mapped == m_frameCount)
    {
        m_damage->Add(m_planes->Session().Bounds());
        m_planes->Session().Configure(m_bounds);
        m_planes->Session().SetBuffer(m_buffer);
        m_damage->Add(m_bounds);
    }
    return true;
}
bool DisplaySession::Create(const System::SystemCallClient& client, const Session::SessionFaceMessage& message) noexcept
{
    const auto* w = message.Words;
    const Foundation::UInt64 x = static_cast<Foundation::UInt32>(w[7U]);
    const Foundation::UInt64 y = w[7U] >> 32U;
    if ((m_memory.Selector != 0U) || (w[1U] > Abi::ComponentSessionMaximumPages) ||
        (x + static_cast<Foundation::UInt32>(w[4U]) > m_info->Width) || (y + (w[4U] >> 32U) > m_info->Height) ||
        (w[6U] != static_cast<Foundation::UInt64>(m_buffer.Format)))
    {
        return false;
    }
    const auto memory = CreateReadMemory(client, m_domain, w[1U]);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_memory = {memory.Value0, memory.Value1};
    m_frameCount = static_cast<Foundation::Size>(w[2U]);
    Foundation::UInt64 offset{0U};
    for (Foundation::Size i{0U}; i < m_frameCount; ++i)
    {
        m_pages[i] = (w[8U + (i >> 2U)] >> ((i & 3U) * 16U)) & 0xFFFFU;
        m_offsets[i] = offset;
        offset += m_pages[i];
    }
    m_bounds = {.X = static_cast<Foundation::Int32>(x),
                .Y = static_cast<Foundation::Int32>(y),
                .Width = static_cast<Foundation::UInt32>(w[4U]),
                .Height = static_cast<Foundation::UInt32>(w[4U] >> 32U)};
    m_buffer = {.Pixels = reinterpret_cast<const Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(m_address)),
                .Width = m_bounds.Width,
                .Height = m_bounds.Height,
                .Stride = static_cast<Foundation::UInt32>(w[5U]),
                .Format = m_buffer.Format};
    return Add(client, message, static_cast<Foundation::Size>(w[3U]));
}
void DisplaySession::CloseCredit(const System::SystemCallClient& client) noexcept
{
    if (m_credit.Selector != 0U)
    {
        static_cast<void>(client.CloseCapability(m_credit));
    }
    m_credit = {};
    m_frozenCount = 0U;
    m_staleCredit = false;
}
bool DisplaySession::Dispatch(const System::SystemCallClient& client, const Session::SessionFaceMessage& message,
                              const Foundation::UInt64 rights, const Foundation::Size route) noexcept
{
    const auto method = message.Method;
    const bool manage = (rights & EchoManageRight) != 0U;
    const bool invoke = (rights & EchoInvokeRight) != 0U;
    const bool authorized =
        (method == Session::FaceOpen) ? (route == 0U && manage)
        : (method == Session::FaceDetach)
            ? (route == 2U && invoke)
            : (route == 1U && ((method == Session::FaceShow || method == Session::FaceDrop) ? manage : invoke));
    const bool epoch = method == Session::FaceShow || method == Session::FaceDrop || message.Words[0U] == m_info->Epoch;
    if (!authorized || !epoch)
    {
        if (authorized && (method == Session::FaceOpen || method == Session::FacePresent))
        {
            if (!Reply(client, message.Attachments[0U], message.Transaction, Session::SessionStale))
            {
                static_cast<void>(client.CloseCapability(message.Attachments[0U]));
            }
            return true;
        }
        return false;
    }
    switch (method)
    {
    case Session::FaceOpen:
    {
        const bool opened = Open(client);
        m_routing = opened;
        if (!Reply(client, message.Attachments[0U], message.Transaction, opened ? DisplayDone : DisplayRefused, opened))
        {
            static_cast<void>(client.CloseCapability(message.Attachments[0U]));
        }
        return true;
    }
    case Session::FaceCreateSurface:
        return Create(client, message);
    case Session::FaceAddSurfaceFrames:
        return Add(client, message, static_cast<Foundation::Size>(message.Words[1U]));
    case Session::FacePresent:
    {
        Foundation::UInt64 kind = Pending() ? Session::SessionBusy : DisplayRefused;
        bool valid = !Pending() && m_frameCount != 0U && m_mapped == m_frameCount && message.Words[1U] > m_serial;
        for (Foundation::Size i{0U}; i < message.Words[2U]; ++i)
        {
            const auto rect = Session::UnpackSessionDamage(message.Words[4U + i]);
            valid = valid && (static_cast<Foundation::UInt64>(rect.X) + rect.Width <= m_bounds.Width) &&
                    (static_cast<Foundation::UInt64>(rect.Y) + rect.Height <= m_bounds.Height);
        }
        if (!valid)
        {
            if (!Reply(client, message.Attachments[0U], message.Transaction, kind))
            {
                static_cast<void>(client.CloseCapability(message.Attachments[0U]));
            }
            return true;
        }
        m_presentKind = Session::SessionPresented;
        m_present = message.Attachments[0U];
        m_presentTransaction = message.Transaction;
        m_serial = message.Words[1U];
        for (Foundation::Size i{0U}; i < message.Words[2U]; ++i)
        {
            const auto r = Session::UnpackSessionDamage(message.Words[4U + i]);
            m_damage->Add({.X = m_bounds.X + r.X, .Y = m_bounds.Y + r.Y, .Width = r.Width, .Height = r.Height});
        }
        return true;
    }
    case Session::FaceShow:
        m_planes->Session().Show(message.Words[0U] != 0U);
        m_planes->Console().Show(message.Words[0U] == 0U);
        m_damage->Add({.X = 0, .Y = 0, .Width = m_info->Width, .Height = m_info->Height});
        return true;
    case Session::FaceFence:
        // An owner fences the stopped producer's same receive route before Detach, so no queued old
        // credit can arrive afterwards.
        return Reply(client, message.Attachments[0U], message.Transaction,
                     static_cast<Foundation::UInt64>(Session::SessionAnswer::Done));
    case Session::FaceCredit:
        // A second credit cannot cancel an immutable WouldBlock answer.
        if (m_credit.Selector != 0U)
        {
            return false;
        }
        m_routing = true;
        m_credit = message.Attachments[0U];
        m_creditTransaction = message.Transaction;
        m_creditMaximum = static_cast<Foundation::Size>(message.Words[1U]);
        return true;
    case Session::FaceDrop:
        m_planes->Session().Show(false);
        m_planes->Console().Show(true);
        m_damage->Add({.X = 0, .Y = 0, .Width = m_info->Width, .Height = m_info->Height});
        CloseCredit(client);
        m_routing = false;
        m_pointerStale = false;
        m_count = 0U;
        [[fallthrough]];
    case Session::FaceDetach:
    case Session::FaceDestroySurface:
        // A detached producer restarts its Present serial at one.
        if (method == Session::FaceDetach)
        {
            m_serial = 0U;
            CloseCredit(client);
            m_dropped += m_count;
            m_count = 0U;
            m_pointerStale = false;
        }
        if (Pending())
        {
            m_presentKind = DisplayRefused;
            if (Reply(client, m_present, m_presentTransaction, m_presentKind))
            {
                m_present = {};
            }
        }
        return Release(client);
    default:
        return false;
    }
}
void DisplaySession::Pointer(const Foundation::UInt64* const words, const Graphics::Point position) noexcept
{
    if (!m_routing || Trusted())
    {
        return;
    }
    if (m_pointerStale)
    {
        ++m_dropped;
        return;
    }
    // Preserve buttons and key stamps. Merge only adjacent motion with the
    // same key stamp; wheel and button changes remain separate.
    if ((m_count > m_frozenCount) && words[2U] != DisplayPointerSnapshot && words[6U] == 0U)
    {
        auto* const last = m_snapshots[(m_first + m_count - 1U) % SnapshotCapacity];
        if (m_motion[(m_first + m_count - 1U) % SnapshotCapacity] && (last[3U] == words[7U]) && (last[2U] == words[4U]))
        {
            last[0U] = words[1U];
            last[1U] = static_cast<Foundation::UInt32>(position.X) |
                       (static_cast<Foundation::UInt64>(static_cast<Foundation::UInt32>(position.Y)) << 32U);
            return;
        }
    }
    if (m_count == SnapshotCapacity)
    {
        // A stopped compositor must never back-pressure the broker's cursor or trusted console.
        // Keep the frozen prefix; count and drop the rest, and report Stale on the next unfrozen
        // credit.
        m_dropped += m_count - m_frozenCount + 1U;
        m_count = m_frozenCount;
        m_pointerStale = true;
        return;
    }
    const auto at = (m_first + m_count++) % SnapshotCapacity;
    m_motion[at] = words[2U] != DisplayPointerSnapshot && words[6U] == 0U;
    auto* const snapshot = m_snapshots[at];
    snapshot[0U] = words[1U];
    snapshot[1U] = static_cast<Foundation::UInt32>(position.X) |
                   (static_cast<Foundation::UInt64>(static_cast<Foundation::UInt32>(position.Y)) << 32U);
    snapshot[2U] = words[4U] | (words[6U] << 32U);
    snapshot[3U] = words[7U];
}
void DisplaySession::SendCredit(const System::SystemCallClient& client) noexcept
{
    if (Trusted() || m_credit.Selector == 0U || (m_count == 0U && !m_pointerStale))
    {
        return;
    }
    if (m_frozenCount == 0U && !m_staleCredit && m_pointerStale)
    {
        for (auto& word : m_words)
        {
            word = 0U;
        }
        m_words[0U] = m_creditTransaction;
        m_words[1U] = Session::SessionStale;
        m_words[2U] = m_info->Version;
        m_words[3U] = m_info->Features;
        m_words[4U] = Session::PackSessionPair(m_info->Width, m_info->Height);
        m_words[5U] = Session::PackSessionPair(m_info->Columns, m_info->Rows);
        m_words[6U] = m_info->Epoch;
        m_creditBytes = Session::WriteSessionFaceFrame(m_creditFrame, DisplayReplyMethod, m_creditTransaction, m_words,
                                                       nullptr, 0U);
        m_staleCredit = true;
    }
    if (m_frozenCount == 0U && !m_staleCredit)
    {
        for (auto& word : m_words)
        {
            word = 0U;
        }
        m_frozenCount = m_count < m_creditMaximum ? m_count : m_creditMaximum;
        m_words[0U] = m_info->Epoch;
        m_words[1U] = m_frozenCount;
        for (Foundation::Size i{0U}; i < m_frozenCount; ++i)
        {
            for (Foundation::Size w{0U}; w < 4U; ++w)
            {
                m_words[4U + i * 4U + w] = m_snapshots[(m_first + i) % SnapshotCapacity][w];
            }
        }
        m_creditBytes = Session::WriteSessionFaceFrame(m_creditFrame, Session::FaceSnapshots, m_creditTransaction,
                                                       m_words, nullptr, 0U);
    }
    const auto count = m_frozenCount;
    const auto error = client.Send(m_credit, System::AddressOf(m_creditFrame), m_creditBytes).Error;
    if (error == Abi::SystemCallError::WouldBlock)
    {
        return;
    }
    CloseCredit(client);
    if (error == Abi::SystemCallError::None)
    {
        m_first = (m_first + count) % SnapshotCapacity;
        m_count -= count;
    }
}
void DisplaySession::Presented(const System::SystemCallClient& client) noexcept
{
    if (m_trustedOpenReply.Selector != 0U)
    {
        for (auto& word : m_words)
        {
            word = 0U;
        }
        m_words[0U] = m_info->Epoch;
        const auto size =
            Identity::WriteSeatFrame(m_frame, Identity::SeatFace::TrustedDraw, Identity::SeatMethod::DrawOpened,
                                     m_trustedOpenTransaction, m_words, m_trustedFace);
        const auto error = client.Send(m_trustedOpenReply, System::AddressOf(m_frame), size).Error;
        if (error != Abi::SystemCallError::WouldBlock)
        {
            static_cast<void>(client.CloseCapability(m_trustedOpenReply));
            m_trustedOpenReply = {};
            if (error != Abi::SystemCallError::None)
            {
                m_failed = true;
            }
        }
    }
    if (m_trustedTransaction != 0U)
    {
        // Render has finished before acknowledging reuse of the public pixels.
        m_trustedPresented = true;
        for (auto& word : m_words)
        {
            word = 0U;
        }
        m_words[0U] = m_enrollment.Epoch();
        m_words[1U] = static_cast<Foundation::UInt64>(Identity::Status::Ok);
        m_words[2U] = m_trustedSerial;
        const auto size = Identity::WriteSeatFrame(m_frame, Identity::SeatFace::LoginControl,
                                                   Identity::SeatMethod::Reply, m_trustedTransaction, m_words);
        const auto error = client.Send(m_trustedFeedback, System::AddressOf(m_frame), size).Error;
        if (error == Abi::SystemCallError::None)
        {
            m_trustedTransaction = 0U;
            m_trustedPresented = false;
        }
        else if (error != Abi::SystemCallError::WouldBlock)
        {
            m_failed = true;
        }
    }
    if (Pending())
    {
        if (Reply(client, m_present, m_presentTransaction, m_presentKind))
        {
            m_present = {};
        }
    }
}
} // Namespace Asteria::User::Component

namespace Asteria::User::Component
{
bool DisplaySession::DispatchTrusted(const System::SystemCallClient& client, const Identity::SeatMessage& message,
                                     Foundation::Size route) noexcept
{
    namespace I = Identity;
    if (message.Face != I::SeatFace::TrustedDraw)
    {
        return false;
    }
    const auto* w = message.Words;
    if (message.Method == I::SeatMethod::OpenDraw)
    {
        if (route != 0U || (message.Rights & EchoManageRight) == 0U || w[0U] != m_info->Epoch)
        {
            return false;
        }
        if (m_trustedOpenReply.Selector != 0U)
        {
            return false;
        }
        if (m_trustedFace.Selector == 0U)
        {
            const auto face = CreateEndpoint(client, m_domain, m_waitSet, TrustedTag);
            if (face.Error != Abi::SystemCallError::None)
            {
                return false;
            }
            m_trustedFace = {face.Value0, face.Value1};
        }
        m_trustedOpenReply = message.Attachment;
        m_trustedOpenTransaction = message.Transaction;
        return true; // Presented sends/retries the frozen root-only handover.
    }
    const bool root = route == 3U && (message.Rights & EchoManageRight) != 0U;
    if (message.Method == I::SeatMethod::Enroll)
    {
        if (!root || !m_enrollment.Enroll(w[1U], w[0U]))
        {
            return false;
        }
        if (m_trustedFeedback.Selector != 0U)
        {
            static_cast<void>(client.CloseCapability(m_trustedFeedback));
        }
        m_trustedFeedback = message.Attachment;
        m_trustedSerial = 0U;
        m_trustedTransaction = 0U;
        m_trustedPresented = false;
        m_planes->Trusted().SetBuffer({});
        // A superseded producer cannot leave a mapped buffer behind.
        if (m_trustedFrame.Selector != 0U)
        {
            if (client.UnmapMemory(m_space, m_address + Abi::ComponentSessionMaximumPages * 4096U, I::LoginPixelsPages)
                    .Error != Abi::SystemCallError::None)
            {
                m_failed = true;
                return false;
            }
            static_cast<void>(client.CloseCapability(m_trustedFrame));
            m_trustedFrame = {};
        }
        return true;
    }
    if (w[0U] != m_enrollment.Epoch())
    {
        return false;
    }
    if (message.Method == I::SeatMethod::Show || message.Method == I::SeatMethod::Hide)
    {
        if (!root)
        {
            return false;
        }
        m_planes->Trusted().Show(message.Method == I::SeatMethod::Show);
        if (Trusted())
        {
            m_first = 0U;
            m_count = 0U;
            m_frozenCount = 0U;
            m_creditBytes = 0U;
        }
        m_damage->Add({.X = 0, .Y = 0, .Width = m_info->Width, .Height = m_info->Height});
        return true;
    }
    if (route != 3U || message.Method != I::SeatMethod::Draw || !m_enrollment.Accepts(message.Sender, w[0U]) ||
        m_trustedTransaction != 0U || w[1U] <= m_trustedSerial ||
        w[2U] != static_cast<Foundation::UInt64>(m_buffer.Format))
    {
        return false;
    }
    if (m_trustedMemory.Selector == 0U)
    {
        const auto memory = CreateReadMemory(client, m_domain, I::LoginPixelsPages);
        if (memory.Error != Abi::SystemCallError::None)
        {
            m_failed = true;
            return false;
        }
        m_trustedMemory = {memory.Value0, memory.Value1};
    }
    const auto address = m_address + Abi::ComponentSessionMaximumPages * 4096U;
    if (m_trustedFrame.Selector == 0U)
    {
        if (MapRead(client, m_space, m_trustedMemory, message.Attachment, address, I::LoginPixelsPages, 0U) !=
            Abi::SystemCallError::None)
        {
            m_failed = true;
            return false;
        }
        m_trustedFrame = message.Attachment;
    }
    else
    {
        static_cast<void>(client.CloseCapability(message.Attachment));
    }
    m_trustedSerial = w[1U];
    m_trustedTransaction = message.Transaction;
    m_planes->Trusted().SetBuffer(
        {.Pixels = reinterpret_cast<const Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(address)),
         .Width = I::LoginWidth,
         .Height = I::LoginHeight,
         .Stride = I::LoginWidth,
         .Format = m_buffer.Format});
    m_damage->Add({.X = 0, .Y = 0, .Width = m_info->Width, .Height = m_info->Height});
    return true;
}
} // Namespace Asteria::User::Component
