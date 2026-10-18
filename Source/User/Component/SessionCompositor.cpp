// SPDX-License-Identifier: Apache-2.0

#include "User/Component/SessionCompositor.hpp"

#include "Abi/Clock.hpp"
#include "User/Component/SessionLiveness.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Graphics/Composition/Divide.hpp"
#include "Runtime/Placement.hpp"
namespace Asteria::User::Component
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt32;
using Foundation::UInt8;
using System::CapabilityHandle;
using K = Session::SessionEventKind;
constexpr Foundation::UInt8 ReadUser{0x09U};
constexpr Size TitleTranscriptBytes{sizeof("# session: title ") - 1U + 2U + 1U + 2U * Session::SessionMaximumNameBytes +
                                    1U + 2U * Session::SessionMaximumSubtitleBytes + 1U};
static_assert(Session::Scene::MaximumWindows <= 100U);
static_assert(TitleTranscriptBytes <= DisplayMaximumWriteBytes);
[[nodiscard, gnu::noinline]] Abi::SystemCallResponse
ReadMemory(const System::SystemCallClient& client, const CapabilityHandle domain, const UInt64 pages) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateMemoryObjectRequestSizeBytes]{};
    System::StoreWord(request + Abi::ObjectCreatePagesOffset, pages);
    request[Abi::ObjectCreatePermissionsOffset] = ReadUser;
    request[Abi::ObjectCreateCachePolicyOffset] = Abi::ObjectCreateCacheWriteBack;
    return client.CreateObject(domain, Abi::ObjectTypeMemoryObject, System::AddressOf(request), sizeof(request));
}
[[nodiscard, gnu::noinline]] Abi::SystemCallError MapRead(const System::SystemCallClient& client,
                                                          const CapabilityHandle space, const CapabilityHandle memory,
                                                          const CapabilityHandle frame, const UInt64 address,
                                                          const UInt64 pages, const UInt64 offset) noexcept
{
    // MemoryObject offset advances; every attached Frame starts
    // at page zero. The client's advertised length is checked by MemoryMap.
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
[[nodiscard, gnu::noinline]] Abi::SystemCallError MapOutput(const System::SystemCallClient& client,
                                                            const CapabilityHandle space, const CapabilityHandle memory,
                                                            const CapabilityHandle frame, const UInt64 address,
                                                            const UInt64 pages, const UInt64 offset) noexcept
{
    // One output MemoryObject, bounded Frames.
    alignas(8) Foundation::UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
    return client.MapMemory(System::AddressOf(request)).Error;
}
[[nodiscard, gnu::noinline]] bool ProbePoolWrite(const System::SystemCallClient& client, const CapabilityHandle domain,
                                                 const CapabilityHandle space, const CapabilityHandle frame,
                                                 const UInt64 address) noexcept
{
    // Use a writable object so only the received Frame's
    // Map-only authority can cause RightsDenied. No pool mapping exists yet.
    const auto created = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, 1U);
    if (created.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    const CapabilityHandle memory{created.Value0, created.Value1};
    const auto error = System::MapWindow(client, space, memory, frame, address, 1U);
    if (error == Abi::SystemCallError::None &&
        client.UnmapMemory(space, address, 1U).Error != Abi::SystemCallError::None)
    {
        return false;
    }
    const auto destroyed = client.DestroyObject(memory, Abi::ObjectTypeMemoryObject).Error;
    const auto closed = client.CloseCapability(memory).Error;
    return error == Abi::SystemCallError::RightsDenied && destroyed == Abi::SystemCallError::None &&
           closed == Abi::SystemCallError::None;
}
[[nodiscard, gnu::noinline]] Abi::SystemCallResponse Endpoint(const System::SystemCallClient& client,
                                                              const CapabilityHandle domain,
                                                              const CapabilityHandle wait, const UInt64 id) noexcept
{
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateEndpointRequestSizeBytes]{};
    System::BuildDefaultEndpointRequest(request);
    System::StoreWord(request + Abi::ObjectCreateEndpointSealedSelectorOffset, Session::SessionClientSelector | id);
    const auto created = client.CreateEndpoint(domain, System::AddressOf(request));
    if (created.Error != Abi::SystemCallError::None)
    {
        return created;
    }
    const CapabilityHandle handle{created.Value0, created.Value1};
    const auto bound = client.BindWaitSet(wait, handle, id);
    if (bound.Error != Abi::SystemCallError::None)
    {
        static_cast<void>(client.DestroyObject(handle, Abi::ObjectTypeEndpoint));
        static_cast<void>(client.CloseCapability(handle));
        return bound;
    }
    return created;
}
void Clear(UInt64* words) noexcept
{
    for (Size i{0U}; i < Session::SessionMaximumWords; ++i)
    {
        words[i] = 0U;
    }
}
[[nodiscard]] CapabilityHandle Handle(const UInt64* block, const Size at) noexcept
{
    return {block[at], block[at + 1U]};
}
// DebugWrite's complete ASCII lines carry UTF-8 text in hex, without changing
// title bytes. The runner replays these snapshots only after Presented.
void Text(char* out, Size& count, const char* text) noexcept
{
    for (Size i{0U}; i < Abi::DebugWriteMaximumBytes && text[i] != '\0' && count + 1U < Abi::DebugWriteMaximumBytes;
         ++i)
    {
        out[count++] = text[i];
    }
}
void Number(char* out, Size& count, UInt64 number) noexcept
{
    char digits[20U]{};
    Size used{0U};
    do
    {
        const auto quotient = Graphics::Composition::DivideCounter(number, 10U);
        digits[used++] = static_cast<char>('0' + number - quotient * 10U);
        number = quotient;
    } while (number != 0U && used < sizeof(digits));
    while (used != 0U && count + 1U < Abi::DebugWriteMaximumBytes)
    {
        out[count++] = digits[--used];
    }
}
void Signed(char* out, Size& count, const Foundation::Int32 number) noexcept
{
    if (number < 0)
    {
        Text(out, count, "-");
        Number(out, count, static_cast<UInt64>(-static_cast<Foundation::Int64>(number)));
    }
    else
    {
        Number(out, count, static_cast<UInt64>(number));
    }
}
void Hex(char* out, Size& count, const Foundation::UInt8* bytes, const Size length) noexcept
{
    constexpr char digits[]{"0123456789abcdef"};
    if (length == 0U)
    {
        Text(out, count, "-");
    }
    for (Size i{0U}; i < length && count + 2U < Abi::DebugWriteMaximumBytes; ++i)
    {
        out[count++] = digits[bytes[i] >> 4U];
        out[count++] = digits[bytes[i] & 15U];
    }
}
alignas(SessionCompositor) constinit Foundation::UInt8 g_storage[sizeof(SessionCompositor)]{};
constinit SessionCompositor* g_instance{nullptr};
} // Namespace
Foundation::UInt64 SessionCompositor::Now() const noexcept
{
    return m_clock == nullptr ? 0U : Graphics::Composition::DivideCounter(m_clock(), m_scale);
}
bool SessionCompositor::Start(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                              const SessionClock clock, const UInt64 regionBase) noexcept
{
    m_domain = Handle(block, Abi::ComponentAccountWord);
    m_space = Handle(block, Abi::ComponentAccountSpaceWord);
    m_wait = Handle(block, Abi::ComponentWaitSetWord);
    m_main = Handle(block, Abi::ComponentReceiveWord);
    m_reply = Handle(block, Abi::ComponentReplyWord);
    m_face = Handle(block, Abi::ComponentInputServiceWord);
    m_report = Handle(block, Abi::ComponentSessionReportWord);
    m_epoch = block[Abi::ComponentInputEpochWord];
    m_input = Handle(block, Abi::ComponentSessionInputWord);
    m_inputEpoch = block[Abi::ComponentSessionInputEpochWord];
    m_inputLive = m_input.Selector != 0U && m_inputEpoch != 0U;
    m_faceLive = m_inputLive;
    m_order.Configure(m_inputEpoch, m_epoch);
    m_scale = block[Abi::ComponentClockTicksPerMicrosecondWord];
    m_clock = clock;
    m_testPoolWrite = (block[Abi::ComponentMountFlagsWord] & SessionTestPoolWriteFlag) != 0U;
    m_testSlowPresent = (block[Abi::ComponentMountFlagsWord] & SessionTestSlowPresentFlag) != 0U;
#if defined(ASTERIA_TARGET_IA64)
    m_base = Abi::Ia64ComponentCompositorAddress;
#else
    m_base = Abi::ComponentCompositorAddress;
#endif
    if (regionBase != 0U)
    {
        m_base = regionBase;
    }
    if (m_epoch == 0U || m_scale == 0U || !Session::IsAddressRegionBase(m_base))
    {
        return false;
    }
    // The output follows the supplied native mode; the session test media supplies its small mode
    // explicitly.
    const auto mode = block[Abi::ComponentSessionOutputModeWord];
    const auto width = static_cast<Foundation::UInt32>(mode);
    const auto height = static_cast<Foundation::UInt32>(mode >> 32U);
    const UInt64 pages = (static_cast<UInt64>(width) * height * 4U + 4095U) / 4096U;
    if (width == 0U || height == 0U || width > Session::SessionMaximumExtent ||
        height > Session::SessionMaximumExtent || pages > Abi::ComponentSessionMaximumPages)
    {
        return false;
    }
    const auto memory = System::CreateMemory(client, m_domain, Abi::ObjectTypeMemoryObject, pages);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_outputMemory = {memory.Value0, memory.Value1};
    // Hot output precedes all client mappings on IA-64.
    const auto address = m_base + Session::AddressOutputOffset;
    const Size count =
        static_cast<Size>((pages + Session::SessionMaximumFramePages - 1U) / Session::SessionMaximumFramePages);
    UInt64 offset{0U};
    Clear(m_words);
    for (Size i{0U}; i < count; ++i)
    {
        const UInt64 remaining = pages - offset;
        const UInt64 chunk =
            remaining < Session::SessionMaximumFramePages ? remaining : Session::SessionMaximumFramePages;
        const auto frame = chunk > Abi::ObjectCreateFrameLegacyMaximumPages
                               ? System::CreateRamFrame(client, m_domain, chunk)
                               : System::CreateMemory(client, m_domain, Abi::ObjectTypeFrame, chunk);
        if (frame.Error != Abi::SystemCallError::None)
        {
            return false;
        }
        m_outputFrames[i] = {frame.Value0, frame.Value1};
        if (MapOutput(client, m_space, m_outputMemory, m_outputFrames[i], address + offset * Session::SessionPageBytes,
                      chunk, offset) != Abi::SystemCallError::None)
        {
            return false;
        }
        m_words[8U + (i >> 2U)] |= chunk << ((i & 3U) * 16U);
        offset += chunk;
    }
    const auto format = static_cast<Graphics::PixelFormat>(block[Abi::ComponentMountFlagsWord]);
    m_output = {.Pixels = reinterpret_cast<UInt32*>(static_cast<Foundation::UIntPointer>(address)),
                .Width = width,
                .Height = height,
                .Stride = width,
                .Format = format};
    if (!LoadUiFonts(client, block))
    {
        return false;
    }
    m_scene.SetUiText(&m_ui);
    m_scene.Configure(width, height, format);
    m_manager.Configure(m_scene, m_events);
    m_words[0U] = m_epoch;
    m_words[1U] = pages;
    m_words[2U] = count;
    m_words[4U] = Session::PackSessionPair(width, height);
    m_words[5U] = width;
    m_words[6U] = static_cast<UInt64>(format);
    const Size firstCount = count < Session::SessionMaximumAttachments ? count : Session::SessionMaximumAttachments;
    const auto bytes =
        Session::WriteSessionFaceFrame(m_frame, Session::FaceCreateSurface, 1U, m_words, m_outputFrames, firstCount);
    if (bytes == 0U || client.Send(m_face, System::AddressOf(m_frame), bytes).Error != Abi::SystemCallError::None)
    {
        return false;
    }
    for (Size first{firstCount}; first < count; first += Session::SessionMaximumAttachments)
    {
        const Size remaining = count - first;
        const Size attached =
            remaining < Session::SessionMaximumAttachments ? remaining : Session::SessionMaximumAttachments;
        Clear(m_words);
        m_words[0U] = m_epoch;
        m_words[1U] = first;
        const auto added = Session::WriteSessionFaceFrame(m_frame, Session::FaceAddSurfaceFrames, 1U + first, m_words,
                                                          m_outputFrames + first, attached);
        if (added == 0U || client.Send(m_face, System::AddressOf(m_frame), added).Error != Abi::SystemCallError::None)
        {
            return false;
        }
    }
    return InputCredits(client);
}
bool SessionCompositor::LoadUiFonts(const System::SystemCallClient& client,
                                    const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const auto frame = Handle(block, Abi::ComponentUiFontFrameWord);
    const auto memory = Handle(block, Abi::ComponentUiFontMemoryWord);
    const UInt64 bold{block[Abi::ComponentUiFontBoldBytesWord]};
    const UInt64 regular{block[Abi::ComponentUiFontRegularBytesWord]};
    const UInt64 address{m_base + Session::AddressUiFontOffset};
    if (bold > Abi::ComponentUiFontFaceBytes || regular > Abi::ComponentUiFontFaceBytes)
    {
        return false;
    }
    if (frame.Selector != 0U)
    {
        if (m_testPoolWrite && !ProbePoolWrite(client, m_domain, m_space, frame, address))
        {
            return false;
        }
        m_fontWriteRefused = m_testPoolWrite;
        alignas(8) UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
        System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, m_space);
        System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
        System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
        System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
        System::StoreWord(request + Abi::FaultResolvePageCountOffset, Abi::ComponentUiFontPages);
        System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, block[Abi::ComponentUiFontPageWord]);
        System::StoreWord(request + Abi::FaultResolveFramePageOffset, block[Abi::ComponentUiFontPageWord]);
        request[Abi::FaultResolvePermissionsOffset] = ReadUser;
        if (client.MapMemory(System::AddressOf(request)).Error != Abi::SystemCallError::None)
        {
            return false;
        }
        m_fontMapped = true;
        const auto* const data = reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(address));
        static_cast<void>(m_ui.Open({data, static_cast<Size>(bold)},
                                    {data + Abi::ComponentUiFontFaceBytes, static_cast<Size>(regular)}));
        static_cast<void>(client.CloseCapability(frame));
        static_cast<void>(client.CloseCapability(memory));
    }
    else
    {
        static_cast<void>(m_ui.Open({}, {}));
    }
    return true;
}
bool SessionCompositor::InputCredits(const System::SystemCallClient& client) noexcept
{
    // Downstream client congestion never stops seat credits. A liveness failure drops waiting
    // input; no old-focus fallback.
    if (m_input.Selector != 0U && !m_keyCredit)
    {
        const UInt64 words[4U]{m_inputEpoch, m_inputResumed ? InputMaximumEvents : 0U, 0U, 0U};
        const auto bytes = WriteInputFrame(m_frame, m_inputResumed ? InputCreditMethod : InputResumeMethod,
                                           0x10000000U + m_keyTransaction + 1U, words, nullptr, 0U, &m_reply);
        const auto error = client.Send(m_input, System::AddressOf(m_frame), bytes).Error;
        if (error == Abi::SystemCallError::None)
        {
            ++m_keyTransaction;
            m_keyCredit = true;
        }
        else if (error != Abi::SystemCallError::WouldBlock)
        {
            m_order.Configure(0U, m_epoch);
            static_cast<void>(client.CloseCapability(m_input));
            m_input = {};
            m_inputLive = false;
        }
    }
    if (m_faceLive && m_inputResumed && !m_pointerCredit)
    {
        Clear(m_words);
        m_words[0U] = m_epoch;
        m_words[1U] = Session::SessionMaximumEvents;
        const auto bytes = Session::WriteSessionFaceFrame(
            m_frame, Session::FaceCredit, 0x20000000U + m_pointerTransaction + 1U, m_words, &m_reply, 1U);
        const auto error = client.Send(m_face, System::AddressOf(m_frame), bytes).Error;
        if (error == Abi::SystemCallError::None)
        {
            ++m_pointerTransaction;
            m_pointerCredit = true;
        }
        else if (error != Abi::SystemCallError::WouldBlock)
        {
            m_order.Configure(m_inputEpoch, 0U);
            m_inputLive = false;
            m_faceLive = false;
        }
    }
    return true;
}
void SessionCompositor::Keys(const InputMessage& message) noexcept
{
    m_keyCredit = false;
    for (Size i{0U}; i < message.Count; ++i)
    {
        const auto event = ReadInputEvent(message.Events + i * 24U);
        if (!m_inputLive)
        {
            static_cast<void>(
                m_manager.Shortcut(event.Usage, event.Modifiers, event.Kind == Input::EventKind::KeyDown, Now()));
            ++m_inputDropped;
            continue;
        }
        const Size at = (m_keyFirst + m_keyCount) % InputCapacity;
        if (!m_inputLive || m_keyCount == InputCapacity ||
            (event.Kind != Input::EventKind::KeyDown && event.Kind != Input::EventKind::KeyUp) ||
            !m_order.Keys(message.Words[3U], 1U, at, Now()))
        {
            ++m_inputDropped;
            m_order.Configure(0U, m_epoch);
            m_inputLive = false;
            continue;
        }
        m_keys[at] = event;
        ++m_keyCount;
    }
}
void SessionCompositor::Pointers() noexcept
{
    m_pointerCredit = false;
    for (Size i{0U}; i < m_faceMessage.Words[1U]; ++i)
    {
        const auto* w = m_faceMessage.Words + 4U + i * 4U;
        if (w[0U] <= m_order.AppliedPointer())
        {
            continue;
        } // Old face's fenced snapshots
        const Size at = (m_pointerFirst + m_pointerCount) % InputCapacity;
        if (!m_inputLive || m_pointerCount == InputCapacity || !m_order.Pointer(w[0U], w[3U], at, Now()))
        {
            ++m_inputDropped;
            m_order.Configure(0U, m_epoch);
            m_inputLive = false;
            continue;
        }
        for (Size j{0U}; j < 4U; ++j)
        {
            m_pointers[at][j] = w[j];
        }
        m_pointerSuppressed &= static_cast<Foundation::UInt8>(w[2U]);
        m_pointers[at][2U] &= ~static_cast<UInt64>(m_pointerSuppressed);
        ++m_pointerCount;
    }
}
void SessionCompositor::ApplyInput() noexcept
{
    for (Size i{0U}; i < 2U * InputCapacity; ++i)
    {
        const auto item = m_order.Take(Now());
        if (item.Type == Session::InputOrder::Kind::None)
        {
            break;
        }
        if (item.Type == Session::InputOrder::Kind::Keys)
        {
            const auto& key = m_keys[item.Token];
            static_cast<void>(m_manager.Key(key.Usage, key.Modifiers, key.Kind == Input::EventKind::KeyDown, Now()));
            m_keyFirst = (m_keyFirst + 1U) % InputCapacity;
            --m_keyCount;
        }
        else
        {
            const auto* w = m_pointers[item.Token];
            m_manager.Pointer({Detail::InputSigned(static_cast<UInt32>(w[1U])),
                               Detail::InputSigned(static_cast<UInt32>(w[1U] >> 32U))},
                              static_cast<Foundation::UInt8>(w[2U]),
                              Detail::InputSigned(static_cast<UInt32>(w[2U] >> 32U)), Now());
            m_pointerFirst = (m_pointerFirst + 1U) % InputCapacity;
            --m_pointerCount;
        }
    }
    if (m_order.Dropped() != 0U)
    {
        m_inputLive = false;
    }
}
void SessionCompositor::Event(const Size slot, const K kind, const UInt64 a, const UInt64 b, const UInt64 c,
                              const UInt64 d) noexcept
{
    static_cast<void>(m_events[slot].Queue({kind, {a, b, c, d}}, Now()));
}
void SessionCompositor::Refuse(const Size slot, const Session::SessionRefusal reason) noexcept
{
    Event(slot, K::RequestRefused, m_message.Method, static_cast<UInt64>(reason), 1U);
}
bool SessionCompositor::Lent(const Size slot, const UInt64 image) const noexcept
{
    for (const auto& surface : m_clients[slot].Surfaces)
    {
        if (surface.Image == image)
        {
            return true;
        }
    }
    return false;
}
void SessionCompositor::ReleaseImage(const Size slot, const UInt64 image) noexcept
{
    if (image != 0U && !Lent(slot, image))
    {
        Event(slot, K::ImageReturned, 1ULL << (image - 1U));
    }
}
bool SessionCompositor::ReleasePool(const System::SystemCallClient& client, const Size slot, const Size pool) noexcept
{
    auto& p = m_clients[slot].Pools[pool];
    // Never close a Frame whose mapping survived an error.
    for (Size i{0U}; i < p.Count; ++i)
    {
        if (p.Frames[i].Selector == 0U)
        {
            continue;
        }
        if (client
                .UnmapMemory(m_space,
                             m_base + Session::AddressPlan::Offset(slot, static_cast<UInt32>(pool + 1U)) +
                                 p.Offsets[i] * Session::SessionPageBytes,
                             p.Pages[i])
                .Error != Abi::SystemCallError::None)
        {
            m_failed = true;
            return false;
        }
        static_cast<void>(client.CloseCapability(p.Frames[i]));
        p.Frames[i] = {};
    }
    if (p.Memory.Selector != 0U)
    {
        if (client.DestroyObject(p.Memory, Abi::ObjectTypeMemoryObject).Error != Abi::SystemCallError::None)
        {
            m_failed = true;
            return false;
        }
        static_cast<void>(client.CloseCapability(p.Memory));
        p.Memory = {};
    }
    p.Count = 0U;
    p.Mapped = 0U;
    p.Total = 0U;
    p.Destroy = false;
    return true;
}
bool SessionCompositor::Settle(const System::SystemCallClient& client, const Size slot) noexcept
{
    auto& owner = m_clients[slot];
    for (Size i{0U}; i < Session::SessionMaximumImages; ++i)
    {
        if (owner.Images[i].Forget && !Lent(slot, i + 1U))
        {
            owner.Images[i] = {};
        }
    }
    for (Size p{0U}; p < Session::SessionMaximumPools; ++p)
    {
        if (!owner.Pools[p].Destroy)
        {
            continue;
        }
        bool lent{false};
        for (Size i{0U}; i < Session::SessionMaximumImages; ++i)
        {
            if (owner.Images[i].Pool == p + 1U && Lent(slot, i + 1U))
            {
                lent = true;
            }
        }
        if (lent)
        {
            continue;
        }
        if (!ReleasePool(client, slot, p))
        {
            return false;
        }
        for (auto& image : owner.Images)
        {
            if (image.Pool == p + 1U)
            {
                image = {};
            }
        }
        Event(slot, K::PoolReleased, p + 1U);
        Event(slot, K::PoolDestroyed, p + 1U);
    }
    return true;
}
bool SessionCompositor::Drop(const System::SystemCallClient& client, const Size slot) noexcept
{
    auto& owner = m_clients[slot];
    for (Size s{0U}; s < Session::SessionMaximumSurfaces; ++s)
    {
        m_manager.Unmap(slot * Session::SessionMaximumSurfaces + s, Now());
        owner.Surfaces[s] = {};
    }
    m_manager.ForgetClient(slot);
    for (Size p{0U}; p < Session::SessionMaximumPools; ++p)
    {
        if (!ReleasePool(client, slot, p))
        {
            return false;
        }
    }
    for (auto& image : owner.Images)
    {
        image = {};
    }
    if (owner.Credit.Selector != 0U)
    {
        static_cast<void>(client.CloseCapability(owner.Credit));
        owner.Credit = {};
    }
    if (owner.Endpoint.Selector != 0U)
    {
        if (client.DestroyObject(owner.Endpoint, Abi::ObjectTypeEndpoint).Error != Abi::SystemCallError::None)
        {
            m_failed = true;
            return false;
        }
        static_cast<void>(client.CloseCapability(owner.Endpoint));
        owner.Endpoint = {};
    }
    m_events[slot].Reset();
    return true;
}
bool SessionCompositor::Manager(const System::SystemCallClient& client) noexcept
{
    const auto method = m_message.Method;
    const auto* w = m_message.Words;
    if (method == Session::SessionQueryMethod)
    {
        if (!AnswerSessionLivenessQuery())
        {
            static_cast<void>(client.CloseCapability(m_message.Attachments[0U]));
            return true;
        }
        if (m_watchdogReply.Selector != 0U)
        {
            return false;
        }
        m_watchdogReply = m_message.Attachments[0U];
        m_watchdogTransaction = m_message.Transaction;
        return true;
    }
    if (method == Session::SessionUserLaunchMethod)
    {
        m_manager.UserLaunch(w[0U]);
        return true;
    }
    if (method == Session::SessionDropClientMethod)
    {
        const Size slot = static_cast<Size>((w[0U] & 0xFFU) - 1U);
        auto& owner = m_clients[slot];
        if (owner.Endpoint.Selector == 0U || owner.Generation != w[0U] >> 8U)
        {
            return false;
        }
        // DropClient takes precedence over an unread registration
        // answer. It must release the pool before composition or any reply wait.
        if (owner.Reply.Selector != 0U)
        {
            static_cast<void>(client.CloseCapability(owner.Reply));
            owner.Reply = {};
        }
        if (!Drop(client, slot))
        {
            return false;
        }
        owner.Reply = m_message.Attachments[0U];
        owner.ReplyTransaction = m_message.Transaction;
        owner.ReplyValue = 0U;
        return true;
    }
    if (method != Session::SessionRegisterClientMethod)
    {
        return false;
    }
    Size slot{0U};
    for (; slot < Session::SessionMaximumClients; ++slot)
    {
        if (m_clients[slot].Endpoint.Selector == 0U && m_clients[slot].Reply.Selector == 0U)
        {
            break;
        }
    }
    // A full table is answered, without creating an endpoint or state.
    if (slot == Session::SessionMaximumClients)
    {
        // Keep draining the shared main Endpoint (Presented and seat replies)
        // even if a manager is not reading its bounded refusal reply.
        if (m_refusalReply.Selector != 0U)
        {
            return false;
        }
        m_refusalReply = m_message.Attachments[0U];
        m_refusalTransaction = m_message.Transaction;
        return true;
    }
    auto& owner = m_clients[slot];
    if (owner.Generation == Session::SessionMaximumGeneration)
    {
        return false;
    }
    const UInt64 id = ((owner.Generation + 1U) << 8U) | (slot + 1U);
    const auto endpoint = Endpoint(client, m_domain, m_wait, id);
    if (endpoint.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    ++owner.Generation;
    owner.Endpoint = {endpoint.Value0, endpoint.Value1};
    owner.Launch = w[0U];
    owner.Class = w[1U];
    owner.NameLength = static_cast<Size>(w[2U]);
    for (Size i{0U}; i < owner.NameLength; ++i)
    {
        owner.Name[i] = Session::SessionTextByte(w, 4U, i);
    }
    for (auto& length : owner.SubtitleLength)
    {
        length = 0U;
    }
    m_events[slot].Reset();
    Event(slot, K::Welcome, Session::SessionProtocolVersion, 0U, static_cast<UInt64>(m_output.Format),
          Session::PackSessionPair(m_output.Width, m_output.Height));
    owner.Reply = m_message.Attachments[0U];
    owner.ReplyTransaction = m_message.Transaction;
    owner.ReplyValue = id;
    return true;
}
bool SessionCompositor::Add(const System::SystemCallClient& client, const Size slot, const Size pool,
                            const Size first) noexcept
{
    auto& p = m_clients[slot].Pools[pool];
    if (p.Memory.Selector == 0U || p.Destroy || first != p.Mapped || first + m_message.AttachmentCount > p.Count)
    {
        return false;
    }
    for (Size i{0U}; i < m_message.AttachmentCount; ++i)
    {
        if (p.Frames[first + i].Selector != 0U)
        {
            return false;
        }
    }
    if (m_testPoolWrite)
    {
        if (!ProbePoolWrite(client, m_domain, m_space, m_message.Attachments[0U],
                            m_base + Session::AddressPlan::Offset(slot, static_cast<UInt32>(pool + 1U))))
        {
            return false;
        }
        m_testPoolWrite = false;
        m_poolWriteRefused = true;
    }
    for (Size i{0U}; i < m_message.AttachmentCount; ++i)
    {
        const Size at = first + i;
        if (MapRead(client, m_space, p.Memory, m_message.Attachments[i],
                    m_base + Session::AddressPlan::Offset(slot, static_cast<UInt32>(pool + 1U)) +
                        p.Offsets[at] * Session::SessionPageBytes,
                    p.Pages[at], p.Offsets[at]) != Abi::SystemCallError::None)
        {
            for (Size left{i}; left < m_message.AttachmentCount; ++left)
            {
                static_cast<void>(client.CloseCapability(m_message.Attachments[left]));
            }
            if (!ReleasePool(client, slot, pool))
            {
                return false;
            }
            Event(slot, K::PoolRefused, pool + 1U, static_cast<UInt64>(Session::SessionRefusal::MapFailed));
            return true;
        }
        p.Frames[at] = m_message.Attachments[i];
        ++p.Mapped;
    }
    if (p.Mapped == p.Count)
    {
        Event(slot, K::PoolReady, pool + 1U);
    }
    return true;
}
bool SessionCompositor::Request(const System::SystemCallClient& client, const Size slot) noexcept
{
    auto& owner = m_clients[slot];
    const auto* w = m_message.Words;
    const Size index = static_cast<Size>(w[0U] - 1U);
    switch (m_message.Method)
    {
    case Session::SessionCreditMethod:
        if (w[0U] != m_epoch)
        {
            Refuse(slot, Session::SessionRefusal::Epoch);
            return false;
        }
        if (owner.Credit.Selector != 0U || !m_events[slot].Credit(static_cast<Size>(w[1U]), Now()))
        {
            Refuse(slot, Session::SessionRefusal::State);
            return false;
        }
        owner.Credit = m_message.Attachments[0U];
        owner.CreditTransaction = m_message.Transaction;
        return true;
    case Session::SessionCreatePoolMethod:
    {
        auto& p = owner.Pools[index];
        if (p.Memory.Selector != 0U || w[3U] != 0U)
        {
            break;
        }
        const auto memory = ReadMemory(client, m_domain, w[1U]);
        if (memory.Error != Abi::SystemCallError::None)
        {
            Event(slot, K::PoolRefused, w[0U], static_cast<UInt64>(Session::SessionRefusal::Exhausted));
            return false;
        }
        p.Memory = {memory.Value0, memory.Value1};
        p.Total = w[1U];
        p.Count = static_cast<Size>(w[2U]);
        UInt64 offset{0U};
        for (Size i{0U}; i < p.Count; ++i)
        {
            p.Pages[i] = Session::SessionPoolFramePages(w, i);
            p.Offsets[i] = offset;
            offset += p.Pages[i];
        }
        return Add(client, slot, index, static_cast<Size>(w[3U]));
    }
    case Session::SessionAddPoolFramesMethod:
        if (Add(client, slot, index, static_cast<Size>(w[1U])))
        {
            return true;
        }
        break;
    case Session::SessionDestroyPoolMethod:
        if (owner.Pools[index].Memory.Selector == 0U)
        {
            break;
        }
        owner.Pools[index].Destroy = true;
        return Settle(client, slot);
    case Session::SessionDefineImageMethod:
    {
        const Size image = static_cast<Size>(w[1U] - 1U);
        const auto& p = owner.Pools[index];
        if (p.Memory.Selector == 0U || p.Destroy || p.Mapped != p.Count || Lent(slot, image + 1U))
        {
            break;
        }
        const auto width = static_cast<UInt32>(w[3U]);
        const auto height = static_cast<UInt32>(w[3U] >> 32U);
        const auto stride = static_cast<UInt32>(w[4U]);
        const auto reason = w[5U] != static_cast<UInt64>(m_output.Format) ? Session::SessionRefusal::Format
                                                                          : Session::SessionRefusal::Range;
        if (reason == Session::SessionRefusal::Format ||
            !Session::SessionImageFits(w[2U], width, height, stride, p.Total * Session::SessionPageBytes))
        {
            Event(slot, K::ImageRefused, image + 1U, static_cast<UInt64>(reason));
            Refuse(slot, reason);
            return true;
        }
        auto& defined = owner.Images[image];
        defined.Pool = index + 1U;
        defined.Forget = false;
        defined.View = {.Pixels = reinterpret_cast<const UInt32*>(static_cast<Foundation::UIntPointer>(
                            m_base + Session::AddressPlan::Offset(slot, static_cast<UInt32>(index + 1U)) + w[2U])),
                        .Width = width,
                        .Height = height,
                        .Stride = stride,
                        .Format = m_output.Format};
        return true;
    }
    case Session::SessionForgetImageMethod:
        if (owner.Images[index].Pool == 0U)
        {
            break;
        }
        owner.Images[index].Forget = true;
        return Settle(client, slot);
    case Session::SessionCreateSurfaceMethod:
        if (owner.Surfaces[index].Created)
        {
            break;
        }
        owner.Surfaces[index].Created = true;
        return true;
    case Session::SessionDestroySurfaceMethod:
    {
        auto& surface = owner.Surfaces[index];
        if (!surface.Created)
        {
            break;
        }
        const UInt64 previous = surface.Image;
        surface = {};
        m_manager.Unmap(slot * Session::SessionMaximumSurfaces + index, Now());
        ReleaseImage(slot, previous);
        return Settle(client, slot);
    }
    case Session::SessionPresentMethod:
    {
        auto& surface = owner.Surfaces[index];
        const UInt64 image = w[1U];
        const Size sceneSlot = slot * Session::SessionMaximumSurfaces + index;
        if (!surface.Created || w[2U] <= surface.Serial ||
            (image != 0U && (owner.Images[image - 1U].Pool == 0U || owner.Images[image - 1U].Forget)))
        {
            break;
        }
        const auto previous = surface.Image;
        if (image == 0U)
        {
            m_manager.Unmap(sceneSlot, Now());
        }
        else
        {
            const auto& view = owner.Images[image - 1U].View;
            for (Size i{0U}; i < w[5U]; ++i)
            {
                m_rectangles[i] = Session::UnpackSessionDamage(w[6U + i]);
                if (static_cast<UInt64>(m_rectangles[i].X) + m_rectangles[i].Width > view.Width ||
                    static_cast<UInt64>(m_rectangles[i].Y) + m_rectangles[i].Height > view.Height)
                {
                    Refuse(slot, Session::SessionRefusal::Range);
                    return true;
                }
            }
            if (!m_scene.IsShown(sceneSlot))
            {
                if (!m_manager.Map(sceneSlot, view.Width, view.Height, owner.Launch, Now()))
                {
                    break;
                }
                if (surface.HasHints)
                {
                    static_cast<void>(m_manager.SetHints(sceneSlot, surface.Hints));
                }
                static_cast<void>(m_scene.SetTitle(sceneSlot, owner.Name, owner.NameLength,
                                                   static_cast<Session::ClientClass>(owner.Class)));
                static_cast<void>(m_scene.SetSubtitle(sceneSlot, owner.Subtitle[index], owner.SubtitleLength[index]));
            }
            m_scene.Present(sceneSlot, view, m_rectangles, static_cast<Size>(w[5U]));
            m_manager.AcceptArrange(sceneSlot, w[4U], Now());
        }
        surface.Image = image;
        surface.Serial = w[2U];
        if (previous != image)
        {
            ReleaseImage(slot, previous);
        }
        surface.Pending = (w[3U] & Session::SessionWantFrameShown) != 0U ? w[2U] : 0U;
        // Hidden surfaces are paced on the next fence too (<= 100 ms).
        if (image == 0U || (surface.Pending != 0U && !m_scene.HasDamage()))
        {
            m_scene.DamageAll();
        }
        return Settle(client, slot);
    }
    case Session::SessionSetSubtitleMethod:
        if (!owner.Surfaces[index].Created)
        {
            break;
        }
        owner.SubtitleLength[index] = static_cast<Size>(w[1U]);
        for (Size i{0U}; i < owner.SubtitleLength[index]; ++i)
        {
            owner.Subtitle[index][i] = Session::SessionTextByte(w, 2U, i);
        }
        static_cast<void>(m_scene.SetSubtitle(slot * Session::SessionMaximumSurfaces + index, owner.Subtitle[index],
                                              owner.SubtitleLength[index]));
        return true;
    case Session::SessionSetSizeHintsMethod:
    {
        if (!owner.Surfaces[index].Created)
        {
            break;
        }
        const Session::SizeHints hints{static_cast<UInt32>(w[1U]), static_cast<UInt32>(w[1U] >> 32U),
                                       static_cast<UInt32>(w[2U]), static_cast<UInt32>(w[2U] >> 32U),
                                       static_cast<UInt32>(w[3U]), static_cast<UInt32>(w[3U] >> 32U),
                                       static_cast<UInt32>(w[4U]), static_cast<UInt32>(w[4U] >> 32U)};
        owner.Surfaces[index].Hints = hints;
        owner.Surfaces[index].HasHints = true;
        if (!m_scene.IsShown(slot * Session::SessionMaximumSurfaces + index) ||
            m_manager.SetHints(slot * Session::SessionMaximumSurfaces + index, hints))
        {
            return true;
        }
        break;
    }
    default:
        break;
    }
    Refuse(slot, Session::SessionRefusal::State);
    return false;
}
void SessionCompositor::CloseAttachments(const System::SystemCallClient& client, const Size bytes) noexcept
{
    if (bytes < EchoDeliveryHeaderBytes + EchoHeaderBytes)
    {
        return;
    }
    const auto* frame = m_delivery + EchoDeliveryHeaderBytes;
    const Size start = EchoDeliveryHeaderBytes + EchoHeaderBytes + frame[80U] * 8U;
    for (Size i{0U}; i < frame[81U]; ++i)
    {
        const Size at = start + i * EchoAttachmentBytes;
        if (at + EchoAttachmentBytes > bytes)
        {
            return;
        }
        static_cast<void>(
            client.CloseCapability({System::LoadWord(m_delivery + at), System::LoadWord(m_delivery + at + 8U)}));
    }
}
bool SessionCompositor::Receive(const System::SystemCallClient& client, const Size route) noexcept
{
    const CapabilityHandle endpoint = route == 0U ? m_main : m_clients[route - 1U].Endpoint;
    if (endpoint.Selector == 0U)
    {
        return true;
    }
    const auto received = client.Receive(endpoint, System::AddressOf(m_delivery), sizeof(m_delivery));
    if (received.Error == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (received.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    const auto bytes = static_cast<Size>(received.Value1);
    InputMessage input{};
    if (route == 0U && ReadInputFrame(m_delivery, bytes, input))
    {
        if (input.Transaction != 0x10000000U + m_keyTransaction || !m_keyCredit)
        {
            CloseAttachments(client, bytes);
            return true;
        }
        if (input.Method == InputBaselineMethod && input.Words[0U] == m_inputEpoch && !m_inputResumed)
        {
            m_order.Resume(input.Words[1U], input.Words[2U]);
            m_pointerSuppressed = static_cast<Foundation::UInt8>(input.Words[3U]);
            m_inputResumed = true;
            m_keyCredit = false;
        }
        else if (input.Method == InputEventsMethod && input.Words[0U] == m_inputEpoch && m_inputResumed)
        {
            Keys(input);
        }
        else
        {
            m_keyCredit = false;
            m_order.Configure(0U, m_epoch);
            m_inputLive = false;
            CloseAttachments(client, bytes);
        }
        return true;
    }
    if (route == 0U && Session::ReadSessionFaceFrame(m_delivery, bytes, m_faceMessage))
    {
        if (m_faceMessage.Transaction == 0x20000000U + m_pointerTransaction && m_pointerCredit)
        {
            if (m_faceMessage.Method == Session::FaceSnapshots && m_faceMessage.Words[0U] == m_epoch)
            {
                Pointers();
            }
            else
            {
                m_order.Configure(m_inputEpoch, 0U);
                m_inputLive = false;
                m_pointerCredit = false;
                CloseAttachments(client, bytes);
            }
            return true;
        }
        if (!m_pending || !m_sent || m_faceMessage.Method != DisplayReplyMethod ||
            m_faceMessage.Transaction != m_serial + 1U || m_faceMessage.Words[1U] != Session::SessionPresented)
        {
            return false;
        }
        m_presentedAt = Now();
        m_passUs = m_presentedAt - m_passStarted;
        if (m_fullPass)
        {
            m_manager.LastPass(m_passUs);
        }
        m_pending = false;
        m_sent = false;
        Record(m_logging);
        // FrameShown depends only on the broker fence, never on a diagnostic
        // consumer draining its report Endpoint.
        for (Size c{0U}; c < Session::SessionMaximumClients; ++c)
        {
            for (Size s{0U}; s < Session::SessionMaximumSurfaces; ++s)
            {
                auto& surface = m_clients[c].Surfaces[s];
                if (surface.Composed != 0U)
                {
                    Event(c, K::FrameShown, s + 1U, surface.Composed, m_presentedAt);
                    surface.Composed = 0U;
                }
            }
        }
        return true;
    }
    if (!Session::ReadSessionFrame(m_delivery, bytes, m_message))
    {
        CloseAttachments(client, bytes);
        return true;
    }
    const auto rights = m_delivery[14U] | (static_cast<UInt64>(m_delivery[15U]) << 8U);
    const bool authorized =
        route == 0U ? (rights & EchoManageRight) != 0U
                    : (rights & EchoInvokeRight) != 0U && m_message.Method <= Session::SessionLastClientMethod;
    if (!authorized)
    {
        CloseAttachments(client, bytes);
    }
    else if (!(route == 0U ? Manager(client) : Request(client, route - 1U)) && !m_failed)
    {
        CloseAttachments(client, bytes);
    }
    return !m_failed;
}
void SessionCompositor::Answers(const System::SystemCallClient& client) noexcept
{
    if (m_poolWriteRefused)
    {
        constexpr char line[]{"# session-test: client pool writable mapping refused\n"};
        const auto bytes =
            WriteDisplayConsoleWrite(m_frame, 1U, reinterpret_cast<const Foundation::UInt8*>(line), sizeof(line) - 1U);
        const auto error = client.Send(m_report, System::AddressOf(m_frame), bytes).Error;
        if (error == Abi::SystemCallError::None)
        {
            m_poolWriteRefused = false;
        }
        else if (error != Abi::SystemCallError::WouldBlock)
        {
            m_failed = true;
            return;
        }
    }
    const auto now = Now();
    if (m_watchdogReply.Selector != 0U)
    {
        Clear(m_words);
        m_words[0U] = m_watchdogTransaction;
        m_words[1U] = static_cast<UInt64>(Session::SessionAnswer::Done);
        const auto bytes = Session::WriteSessionFrame(m_frame, Session::SessionReplyMethod, m_watchdogTransaction,
                                                      m_words, nullptr, 0U);
        if (client.Send(m_watchdogReply, System::AddressOf(m_frame), bytes).Error != Abi::SystemCallError::WouldBlock)
        {
            static_cast<void>(client.CloseCapability(m_watchdogReply));
            m_watchdogReply = {};
        }
    }
    if (m_refusalReply.Selector != 0U)
    {
        Clear(m_words);
        m_words[0U] = m_refusalTransaction;
        m_words[1U] = static_cast<UInt64>(Session::SessionAnswer::Exhausted);
        const auto bytes = Session::WriteSessionFrame(m_frame, Session::SessionReplyMethod, m_refusalTransaction,
                                                      m_words, nullptr, 0U);
        if (client.Send(m_refusalReply, System::AddressOf(m_frame), bytes).Error != Abi::SystemCallError::WouldBlock)
        {
            static_cast<void>(client.CloseCapability(m_refusalReply));
            m_refusalReply = {};
        }
    }
    for (Size c{0U}; c < Session::SessionMaximumClients; ++c)
    {
        auto& owner = m_clients[c];
        if (owner.Reply.Selector != 0U)
        {
            Clear(m_words);
            m_words[0U] = owner.ReplyTransaction;
            m_words[1U] = static_cast<UInt64>(Session::SessionAnswer::Done);
            m_words[2U] = owner.ReplyValue;
            const auto bytes = Session::WriteSessionFrame(m_frame, Session::SessionReplyMethod, owner.ReplyTransaction,
                                                          m_words, owner.ReplyValue != 0U ? &owner.Endpoint : nullptr,
                                                          owner.ReplyValue != 0U ? 1U : 0U);
            const auto error = client.Send(owner.Reply, System::AddressOf(m_frame), bytes).Error;
            if (error != Abi::SystemCallError::WouldBlock)
            {
                static_cast<void>(client.CloseCapability(owner.Reply));
                owner.Reply = {};
            }
        }
        if (owner.Credit.Selector == 0U)
        {
            continue;
        }
        const auto count = m_events[c].Build(m_answer, now);
        if (count == 0U)
        {
            continue;
        }
        const auto bytes = Session::WriteSessionEvents(
            m_frame, owner.CreditTransaction, m_epoch, m_answer, count,
            m_events[c].AnswerMorePending() ? Session::SessionEventsMorePending : 0U, m_events[c].AnswerDropped());
        const auto error = client.Send(owner.Credit, System::AddressOf(m_frame), bytes).Error;
        if (error == Abi::SystemCallError::None)
        {
            m_events[c].Commit(now);
        }
        else
        {
            m_events[c].SendFailed(error == Abi::SystemCallError::WouldBlock, now);
        }
        if (error != Abi::SystemCallError::WouldBlock)
        {
            static_cast<void>(client.CloseCapability(owner.Credit));
            owner.Credit = {};
        }
    }
}
void SessionCompositor::Capture() noexcept
{
    m_captureCount = m_scene.Count();
    for (Size i{0U}; i < m_captureCount; ++i)
    {
        auto& snapshot = m_captured[i];
        const Size slot = m_scene.At(i);
        const Size c = slot / Session::SessionMaximumSurfaces;
        const Size s = slot % Session::SessionMaximumSurfaces;
        const auto& owner = m_clients[c];
        const auto& surface = owner.Surfaces[s];
        snapshot.Slot = slot;
        snapshot.Frame = m_scene.Frame(slot);
        snapshot.Look = m_scene.Look(slot);
        snapshot.Class = owner.Class;
        snapshot.Serial = surface.Serial;
        snapshot.Width = owner.Images[surface.Image - 1U].View.Width;
        snapshot.Height = owner.Images[surface.Image - 1U].View.Height;
        snapshot.NameLength = owner.NameLength;
        snapshot.SubtitleLength = owner.SubtitleLength[s];
        for (Size j{0U}; j < snapshot.NameLength; ++j)
        {
            snapshot.Name[j] = owner.Name[j];
        }
        for (Size j{0U}; j < snapshot.SubtitleLength; ++j)
        {
            snapshot.Subtitle[j] = owner.Subtitle[s][j];
        }
    }
}
void SessionCompositor::Record(const bool queued) noexcept
{
    // A report consumer cannot pace composition. Freeze the current transcript; coalesce later
    // complete, fenced snapshots separately.
    auto* const destination = queued ? m_queued : m_snapshots;
    for (Size i{0U}; i < m_captureCount; ++i)
    {
        destination[i] = m_captured[i];
    }
    if (queued)
    {
        m_queuedCount = m_captureCount;
        m_queuedSerial = m_serial;
        m_queuedPixels = m_passPixels;
        m_queuedUs = m_passUs;
        m_queuedDropped = m_inputDropped + m_order.Dropped();
        m_logQueued = true;
    }
    else
    {
        m_snapshotCount = m_captureCount;
        m_logSerial = m_serial;
        m_logPixels = m_passPixels;
        m_logUs = m_passUs;
        m_logDropped = m_inputDropped + m_order.Dropped();
        m_logging = true;
        m_logIndex = 0U;
    }
}
bool SessionCompositor::Compose(const System::SystemCallClient& client) noexcept
{
    const auto now = Now();
    if (!m_pending && m_scene.HasDamage() &&
        (m_serial == 0U || Abi::ElapsedClockTicks(now, m_lastCompose) >= SessionComposeIntervalUs))
    {
        m_passStarted = Now();
        static_cast<void>(m_scene.Compose(m_output));
        m_passPixels = 0U;
        Capture();
        Clear(m_words);
        m_words[0U] = m_epoch;
        m_words[1U] = ++m_serial;
        const auto& damage = m_scene.Composed();
        m_words[2U] = damage.Count();
        m_fullPass = damage.Count() == 1U && damage.At(0U).X == 0 && damage.At(0U).Y == 0 &&
                     damage.At(0U).Width == m_output.Width && damage.At(0U).Height == m_output.Height;
        for (Size i{0U}; i < damage.Count(); ++i)
        {
            const auto r = damage.At(i);
            m_passPixels += static_cast<UInt64>(r.Width) * r.Height;
            m_words[4U + i] =
                Session::PackSessionDamage(static_cast<UInt32>(r.X), static_cast<UInt32>(r.Y), r.Width, r.Height);
        }
        for (auto& owner : m_clients)
        {
            for (auto& surface : owner.Surfaces)
            {
                surface.Composed = surface.Pending;
                surface.Pending = 0U;
            }
        }
        System::OrderDmaStorage();
        m_presentBytes =
            Session::WriteSessionFaceFrame(m_present, Session::FacePresent, m_serial + 1U, m_words, &m_reply, 1U);
        if (m_presentBytes == 0U)
        {
            return false;
        }
        m_pending = true;
        m_lastCompose = now;
    }
    if (m_pending && !m_sent)
    {
        if (m_testSlowPresent && now - m_passStarted < SessionTestPresentDelayUs)
        {
            return true;
        }
        const auto error = client.Send(m_face, System::AddressOf(m_present), m_presentBytes).Error;
        if (error == Abi::SystemCallError::WouldBlock)
        {
            return true;
        }
        if (error != Abi::SystemCallError::None)
        {
            return false;
        }
        m_sent = true;
    }
    return true;
}
void SessionCompositor::Log(const System::SystemCallClient& client) noexcept
{
    // Root's bootstrap DiagnosticChannel has Write without Duplicate. Send
    // immutable complete lines to its report relay; no diagnostic authority
    // is amplified. WouldBlock retains the snapshot and line index.
    const Size end = 5U + 2U * m_snapshotCount;
    for (; m_logging && m_logIndex < end; ++m_logIndex)
    {
        Size n{0U};
        if (m_logIndex == 0U)
        {
            Text(m_line, n, "# session: font ");
            Text(m_line, n, m_ui.Scalable(true) ? "Vegur-Bold" : "Bold8x16-fallback");
            Text(m_line, n, " ");
            Text(m_line, n, m_ui.Scalable(false) ? "Vegur-Regular" : "Bold8x16-fallback");
        }
        else if (m_logIndex == 1U)
        {
            Text(m_line, n, m_fontMapped ? "# session: font-map read-only Map" : "# session: font-map absent");
            if (m_fontWriteRefused)
            {
                Text(m_line, n, " write-refused");
            }
        }
        else if (m_logIndex == 2U)
        {
            Text(m_line, n, "# session: begin ");
            Number(m_line, n, m_logSerial);
            Text(m_line, n, " ");
            Number(m_line, n, m_output.Width);
            Text(m_line, n, " ");
            Number(m_line, n, m_output.Height);
        }
        else if (m_logIndex + 1U == end)
        {
            Text(m_line, n, "# session: presented ");
            Number(m_line, n, m_logSerial);
            Text(m_line, n, " pixels ");
            Number(m_line, n, m_logPixels);
            Text(m_line, n, " us ");
            Number(m_line, n, m_logUs);
        }
        else if (m_logIndex + 2U == end)
        {
            Text(m_line, n, "# session: input dropped ");
            Number(m_line, n, m_logDropped);
        }
        else
        {
            const auto& s = m_snapshots[(m_logIndex - 3U) / 2U];
            if ((m_logIndex & 1U) != 0U)
            {
                Text(m_line, n, "# session: window ");
                Number(m_line, n, s.Slot);
                Text(m_line, n, " ");
                Signed(m_line, n, s.Frame.X);
                Text(m_line, n, " ");
                Signed(m_line, n, s.Frame.Y);
                const UInt64 values[]{
                    s.Frame.Width,
                    s.Frame.Height,
                    static_cast<UInt64>(s.Look.State),
                    s.Class,
                    s.Width,
                    s.Height,
                    s.Serial,
                    static_cast<UInt64>(s.Look.NotResponding) | (static_cast<UInt64>(s.Look.Maximized) << 1U) |
                        (static_cast<UInt64>(s.Look.Close == Session::ButtonState::Pressed) << 2U) |
                        (static_cast<UInt64>(s.Look.Maximize == Session::ButtonState::Pressed) << 3U)};
                for (const auto value : values)
                {
                    Text(m_line, n, " ");
                    Number(m_line, n, value);
                }
            }
            else
            {
                Text(m_line, n, "# session: title ");
                Number(m_line, n, s.Slot);
                Text(m_line, n, " ");
                Hex(m_line, n, s.Name, s.NameLength);
                Text(m_line, n, " ");
                Hex(m_line, n, s.Subtitle, s.SubtitleLength);
            }
        }
        m_line[n++] = '\n';
        const auto bytes =
            WriteDisplayConsoleWrite(m_frame, m_logSerial, reinterpret_cast<const Foundation::UInt8*>(m_line), n);
        if (bytes == 0U)
        {
            m_failed = true;
            return;
        }
        const auto error = client.Send(m_report, System::AddressOf(m_frame), bytes).Error;
        if (error == Abi::SystemCallError::WouldBlock)
        {
            return;
        }
        if (error != Abi::SystemCallError::None)
        {
            m_failed = true;
            return;
        }
    }
    if (!m_logging)
    {
        return;
    }
    m_logging = false;
    if (m_logQueued)
    {
        for (Size i{0U}; i < m_queuedCount; ++i)
        {
            m_snapshots[i] = m_queued[i];
        }
        m_snapshotCount = m_queuedCount;
        m_logSerial = m_queuedSerial;
        m_logPixels = m_queuedPixels;
        m_logUs = m_queuedUs;
        m_logDropped = m_queuedDropped;
        m_logQueued = false;
        m_logging = true;
        m_logIndex = 0U;
    }
}

bool SessionCompositor::Step(const System::SystemCallClient& client) noexcept
{
    // Four per client, round robin, <= 32 total per wake. Main control (including DropClient)
    // drains before composing or waiting.
    for (Size round{0U}; round < 4U; ++round)
    {
        for (Size route{0U}; route <= Session::SessionMaximumClients; ++route)
        {
            if (!Receive(client, route))
            {
                return false;
            }
        }
    }
    ApplyInput();
    if (!InputCredits(client))
    {
        return false;
    }
    if (m_logging)
    {
        Log(client);
    }
    m_manager.Tick(Now());
    Answers(client);
    return !m_failed && Compose(client);
}
Foundation::UInt64 ServeSession(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                                const SessionClock clock) noexcept
{
    g_instance = new (static_cast<void*>(g_storage)) SessionCompositor{};
    auto& compositor = *g_instance;
    if (!compositor.Start(client, block, clock))
    {
        return 0xC0DE00D0U;
    }
    const auto wait = Handle(block, Abi::ComponentWaitSetWord);
    for (;;)
    {
        if (!compositor.Step(client))
        {
            return 0xC0DE00D1U;
        }
        const auto error = client.Wait(wait, Session::ClientRetryUs).Error;
        if (error != Abi::SystemCallError::None && error != Abi::SystemCallError::TimedOut)
        {
            return 0xC0DE00D2U;
        }
    }
}
} // Namespace Asteria::User::Component
