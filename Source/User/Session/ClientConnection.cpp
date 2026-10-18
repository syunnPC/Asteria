// SPDX-License-Identifier: Apache-2.0

#include "User/Session/ClientConnection.hpp"
#include "User/Session/AddressPlan.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/Graphics/Composition/Divide.hpp"

namespace Asteria::User::Session
{
using Foundation::UInt64;
using Foundation::UInt32;
using Foundation::Size;
bool ClientImagePlan::Configure(const UInt32 width, const UInt32 height) noexcept
{
    const UInt64 pages =
        (static_cast<UInt64>(width) * height * sizeof(UInt32) + SessionPageBytes - 1U) / SessionPageBytes;
    if (width == 0U || height == 0U || width > SessionMaximumExtent || height > SessionMaximumExtent ||
        pages > SessionMaximumPoolPages)
    {
        return false;
    }
    Width = width;
    Height = height;
    Pages = pages;
    return true;
}
UInt64 ClientConnection::Now() noexcept
{
    return m_clock == nullptr || m_scale == 0U ? 0U : Graphics::Composition::DivideCounter(m_clock(), m_scale);
}
void ClientConnection::WaitUntil(const UInt64 deadline) noexcept
{
    const auto now = Now();
    if (deadline <= now)
    {
        return;
    }
    const UInt64 left = deadline - now;
    // Bounded retry turn; the WaitSet supplies the timer primitive.
    const UInt64 microseconds = left > ClientRetryUs / 1000U ? ClientRetryUs : left * 1000U;
    const auto error = m_client.Wait(m_wait, microseconds).Error;
    if (error != Abi::SystemCallError::None && error != Abi::SystemCallError::TimedOut)
    {
        m_healthy = false;
    }
}
bool ClientConnection::Send(const UInt32 method, const UInt64* const words,
                            const System::CapabilityHandle* const handles, const Size count) noexcept
{
    const Size bytes = WriteSessionFrame(m_frame, method, ++m_transaction, words, handles, count);
    if (!m_healthy || bytes == 0U)
    {
        return false;
    }
    for (Size retry{}; retry < 200U; ++retry)
    {
        const auto error = m_client.Send(m_server, System::AddressOf(m_frame), bytes).Error;
        if (error == Abi::SystemCallError::None)
        {
            return true;
        }
        if (error != Abi::SystemCallError::WouldBlock)
        {
            m_healthy = false;
            return false;
        }
        WaitUntil(Now() + ClientRetryUs / 1000U);
        if (!m_healthy)
        {
            return false;
        }
    }
    m_healthy = false;
    return false;
}
bool ClientConnection::Request(const UInt32 method, const UInt64* const words) noexcept
{
    if (method == SessionPresentMethod)
    {
        System::OrderDmaStorage();
    }
    return Send(method, words);
}
bool ClientConnection::Credit() noexcept
{
    for (auto& word : m_words)
    {
        word = 0U;
    }
    m_words[0U] = m_epoch;
    m_words[1U] = SessionMaximumEvents;
    m_credit = Send(SessionCreditMethod, m_words, &m_reply, 1U);
    return m_credit;
}
bool ClientConnection::Poll(SessionEvent& event) noexcept
{
    if (!m_healthy)
    {
        return false;
    }
    if (m_eventIndex < SessionEventCount(m_message))
    {
        return GetSessionEvent(m_message, m_eventIndex++, event);
    }
    if (!m_credit && !Credit())
    {
        return false;
    }
    const auto received = m_client.Receive(m_receive, System::AddressOf(m_delivery), sizeof(m_delivery));
    if (received.Error == Abi::SystemCallError::WouldBlock)
    {
        return false;
    }
    if (received.Error != Abi::SystemCallError::None ||
        !ReadSessionFrame(m_delivery, static_cast<Size>(received.Value1), m_message) ||
        m_message.Method != SessionEventsMethod || m_message.Words[0U] != m_epoch)
    {
        m_healthy = false;
        return false;
    }
    m_credit = false;
    m_eventIndex = 0U;
    return GetSessionEvent(m_message, m_eventIndex++, event);
}
[[nodiscard, gnu::noinline]] static bool MapOwned(const System::SystemCallClient& client,
                                                  const System::CapabilityHandle space,
                                                  const System::CapabilityHandle memory,
                                                  const System::CapabilityHandle frame, const UInt64 address,
                                                  const UInt64 pages, const UInt64 offset) noexcept
{
    // The producer owns Write; each RAM Frame starts at zero.
    alignas(8) Foundation::UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
    return client.MapMemory(System::AddressOf(request)).Error == Abi::SystemCallError::None;
}
bool ClientConnection::Allocate(const UInt32 pool) noexcept
{
    const auto memory = System::CreateMemory(m_client, m_domain, Abi::ObjectTypeMemoryObject, m_plan.Pages);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return false;
    }
    m_memory[pool] = {memory.Value0, memory.Value1};
    UInt64 offset{};
    for (Size index{}; offset < m_plan.Pages && index < SessionMaximumPoolFrames; ++index)
    {
        UInt64 pages = m_plan.Pages - offset;
        if (pages > SessionMaximumFramePages)
        {
            pages = SessionMaximumFramePages;
        }
        Abi::SystemCallResponse frame{};
        // Fragmentation falls back to smaller runs, within 16 Frames.
        for (Size split{}; split < 10U; ++split)
        {
            frame = System::CreateRamFrame(m_client, m_domain, pages);
            if (frame.Error != Abi::SystemCallError::ResourceExhausted || pages == 1U)
            {
                break;
            }
            pages = (pages + 1U) / 2U;
        }
        if (frame.Error != Abi::SystemCallError::None)
        {
            return false;
        }
        m_frames[pool][index] = {frame.Value0, frame.Value1};
        m_pages[pool][index] = static_cast<Foundation::UInt16>(pages);
        ++m_count[pool];
        if (!MapOwned(m_client, m_space, m_memory[pool], m_frames[pool][index],
                      m_base + pool * AddressPoolSpan + offset * SessionPageBytes, pages, offset))
        {
            return false;
        }
        offset += pages;
    }
    return offset == m_plan.Pages;
}
bool ClientConnection::Attach(const UInt32 pool) noexcept
{
    for (auto& word : m_words)
    {
        word = 0U;
    }
    m_words[0U] = pool + 1U;
    m_words[1U] = m_plan.Pages;
    m_words[2U] = m_count[pool];
    for (Size i{}; i < m_count[pool]; ++i)
    {
        m_words[4U + i / 4U] |= static_cast<UInt64>(m_pages[pool][i]) << ((i % 4U) * 16U);
    }
    const Size first = m_count[pool] < SessionMaximumAttachments ? m_count[pool] : SessionMaximumAttachments;
    if (!Send(SessionCreatePoolMethod, m_words, m_frames[pool], first))
    {
        return false;
    }
    for (Size i{first}; i < m_count[pool]; i += SessionMaximumAttachments)
    {
        for (auto& word : m_words)
        {
            word = 0U;
        }
        m_words[0U] = pool + 1U;
        m_words[1U] = i;
        const Size remaining = m_count[pool] - i;
        if (!Send(SessionAddPoolFramesMethod, m_words, m_frames[pool] + i,
                  remaining < SessionMaximumAttachments ? remaining : SessionMaximumAttachments))
        {
            return false;
        }
    }
    return true;
}
bool ClientConnection::Initialize(const UInt64 (&block)[Abi::ComponentBlockWords], const ClientClock clock) noexcept
{
    m_clock = clock;
    m_scale = block[Abi::ComponentClockTicksPerMicrosecondWord] * 1000U;
    m_server = {block[Abi::ComponentInputServiceWord], block[Abi::ComponentInputServiceWord + 1U]};
    m_reply = {block[Abi::ComponentReplyWord], block[Abi::ComponentReplyWord + 1U]};
    m_receive = {block[Abi::ComponentReceiveWord], block[Abi::ComponentReceiveWord + 1U]};
    m_wait = {block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]};
    m_domain = {block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]};
    m_space = {block[Abi::ComponentAccountSpaceWord], block[Abi::ComponentAccountSpaceWord + 1U]};
    m_epoch = block[Abi::ComponentInputEpochWord];
    m_base = block[Abi::ComponentWindowStartWord];
    const UInt64 mode = block[Abi::ComponentSessionOutputModeWord];
    m_format = static_cast<Graphics::PixelFormat>(static_cast<UInt32>(block[Abi::ComponentMountFlagsWord]));
    if (m_scale == 0U || !m_plan.Configure(static_cast<UInt32>(mode), static_cast<UInt32>(mode >> 32U)))
    {
        return false;
    }
    if (!Credit())
    {
        return false;
    }
    bool welcome{};
    const UInt64 deadline = Now() + 60'000U;
    for (Size turn{}; turn < 6000U && Now() < deadline && !welcome; ++turn)
    {
        SessionEvent event{};
        if (Poll(event))
        {
            welcome = event.Kind == SessionEventKind::Welcome && event.Words[3U] == mode &&
                      event.Words[2U] == static_cast<UInt64>(m_format);
            if (!welcome)
            {
                return false;
            }
        }
        else
        {
            WaitUntil(Now() + 10U);
        }
    }
    if (!welcome || !Allocate(0U) || !Allocate(1U) || !Attach(0U) || !Attach(1U))
    {
        return false;
    }
    UInt32 ready{};
    for (Size turn{}; turn < 6000U && Now() < deadline && ready != 3U; ++turn)
    {
        SessionEvent event{};
        if (Poll(event))
        {
            if (event.Kind != SessionEventKind::PoolReady || event.Words[0U] < 1U || event.Words[0U] > 2U)
            {
                return false;
            }
            ready |= 1U << (event.Words[0U] - 1U);
        }
        else
        {
            WaitUntil(Now() + 10U);
        }
    }
    return m_healthy && ready == 3U;
}
Graphics::MemorySurface ClientConnection::Image(const UInt32 index) noexcept
{
    if (index >= 2U)
    {
        return {};
    }
    return {.Pixels = reinterpret_cast<Foundation::UInt32*>(
                static_cast<Foundation::UIntPointer>(m_base + index * AddressPoolSpan)),
            .Width = m_plan.Width,
            .Height = m_plan.Height,
            .Stride = m_plan.Width,
            .Format = m_format};
}
} // Namespace Asteria::User::Session
