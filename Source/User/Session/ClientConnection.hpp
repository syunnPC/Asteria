// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_CLIENT_CONNECTION_HPP_INCLUDED
#define ASTERIA_USER_SESSION_CLIENT_CONNECTION_HPP_INCLUDED

#include "User/Session/SessionFrame.hpp"
#include "User/Session/ClientEvents.hpp"
#include "User/Graphics/Surface.hpp"
#include "User/System/SystemCallClient.hpp"
#include "Abi/ComponentBootstrap.hpp"

namespace Asteria::User::Session
{
// Two complete native images, one per pool.
struct ClientImagePlan
{
    Foundation::UInt32 Width{};
    Foundation::UInt32 Height{};
    Foundation::UInt64 Pages{};
    [[nodiscard]] bool Configure(Foundation::UInt32 width, Foundation::UInt32 height) noexcept;
};
class IClientConnection
{
public:
    [[nodiscard]] virtual Graphics::MemorySurface Image(Foundation::UInt32 index) noexcept = 0;
    [[nodiscard]] virtual bool Request(Foundation::UInt32 method, const Foundation::UInt64* words) noexcept = 0;
    [[nodiscard]] virtual bool Poll(SessionEvent& event) noexcept = 0;
    virtual void WaitUntil(Foundation::UInt64 milliseconds) noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt64 Now() noexcept = 0;
    [[nodiscard]] virtual bool Healthy() const noexcept = 0;

protected:
    ~IClientConnection() = default;
};
using ClientClock = Foundation::UInt64 (*)() noexcept;
class ClientConnection final : public IClientConnection
{
public:
    explicit ClientConnection(const System::SystemCallClient& client) noexcept : m_client{client}
    {
    }
    [[nodiscard]] bool Initialize(const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                  ClientClock clock) noexcept;
    [[nodiscard]] Graphics::MemorySurface Image(Foundation::UInt32 index) noexcept override;
    [[nodiscard]] bool Request(Foundation::UInt32 method, const Foundation::UInt64* words) noexcept override;
    [[nodiscard]] bool Poll(SessionEvent& event) noexcept override;
    void WaitUntil(Foundation::UInt64 milliseconds) noexcept override;
    [[nodiscard]] Foundation::UInt64 Now() noexcept override;
    [[nodiscard]] bool Healthy() const noexcept override
    {
        return m_healthy;
    }
    [[nodiscard]] Foundation::UInt64 PoolPages() const noexcept
    {
        return m_plan.Pages;
    }
    [[nodiscard]] Foundation::Size PoolFrames(Foundation::UInt32 pool) const noexcept
    {
        return pool < 2U ? m_count[pool] : 0U;
    }

private:
    [[nodiscard]] bool Send(Foundation::UInt32 method, const Foundation::UInt64* words,
                            const System::CapabilityHandle* handles = nullptr, Foundation::Size count = 0U) noexcept;
    [[nodiscard]] bool Credit() noexcept;
    [[nodiscard]] bool Allocate(Foundation::UInt32 pool) noexcept;
    [[nodiscard]] bool Attach(Foundation::UInt32 pool) noexcept;
    const System::SystemCallClient& m_client;
    ClientClock m_clock{};
    ClientImagePlan m_plan{};
    System::CapabilityHandle m_server{};
    System::CapabilityHandle m_reply{};
    System::CapabilityHandle m_receive{};
    System::CapabilityHandle m_wait{};
    System::CapabilityHandle m_domain{};
    System::CapabilityHandle m_space{};
    System::CapabilityHandle m_memory[2U]{};
    System::CapabilityHandle m_frames[2U][SessionMaximumPoolFrames]{};
    Foundation::UInt16 m_pages[2U][SessionMaximumPoolFrames]{};
    Foundation::Size m_count[2U]{};
    Foundation::UInt64 m_base{};
    Foundation::UInt64 m_epoch{};
    Foundation::UInt64 m_scale{};
    Foundation::UInt64 m_transaction{};
    Graphics::PixelFormat m_format{};
    SessionMessage m_message{};
    Foundation::Size m_eventIndex{};
    Foundation::UInt64 m_words[SessionMaximumWords]{};
    alignas(8) Foundation::UInt8 m_frame[SessionKernelFrameLimit]{}, m_delivery[SessionDeliveryCapacity]{};
    bool m_healthy{true};
    bool m_credit{};
};
} // Namespace Asteria::User::Session
#endif
