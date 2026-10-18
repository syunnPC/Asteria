// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_CLIENT_EVENTS_HPP_INCLUDED
#define ASTERIA_USER_SESSION_CLIENT_EVENTS_HPP_INCLUDED
#include "User/Session/SessionFrame.hpp"
namespace Asteria::User::Session
{
/// Keep in static storage. Build freezes an answer; Queue may continue while that answer waits.
/// Commit alone consumes it. WouldBlock retains credit and retries after 10 ms; other failures
/// retain events but discard credit. Time is injected in microseconds.
inline constexpr Foundation::Size ClientInputCapacity{64U};
inline constexpr Foundation::Size ClientHeldCapacity{32U};
inline constexpr Foundation::Size ClientStateCapacity{64U};
inline constexpr Foundation::UInt64 ClientRetryUs{10000U};
inline constexpr Foundation::UInt64 ClientDropTimeoutUs{2000000U};
inline constexpr Foundation::UInt64 ClientTimeoutUs{5000000U};
static_assert(ClientHeldCapacity + SessionMaximumSurfaces < ClientInputCapacity);
class ClientEvents final
{
public:
    void Reset() noexcept;
    [[nodiscard]] bool Queue(const SessionEvent& event, Foundation::UInt64 now) noexcept;
    [[nodiscard]] bool Credit(Foundation::Size maximum, Foundation::UInt64 now) noexcept;
    [[nodiscard]] Foundation::Size Build(SessionEvent* out, Foundation::UInt64 now) noexcept;
    void Commit(Foundation::UInt64 now) noexcept;
    void SendFailed(bool wouldBlock, Foundation::UInt64 now) noexcept;
    void Tick(Foundation::UInt64 now) noexcept;
    void DestroySurface(Foundation::UInt64 surface) noexcept;
    void ReleaseKeys(Foundation::UInt64 surface, Foundation::UInt64 now) noexcept;
    [[nodiscard]] bool NotResponding() const noexcept
    {
        return m_notResponding;
    }
    [[nodiscard]] Foundation::Size Queued() const noexcept
    {
        return m_count;
    }
    [[nodiscard]] Foundation::UInt64 Dropped() const noexcept
    {
        return m_dropped;
    }
    [[nodiscard]] bool MorePending() const noexcept;
    /// Header values of the frozen Build, unchanged across WouldBlock retries.
    [[nodiscard]] Foundation::UInt64 AnswerDropped() const noexcept
    {
        return m_builtDropped;
    }
    [[nodiscard]] bool AnswerMorePending() const noexcept
    {
        return m_builtMore;
    }
    [[nodiscard]] Foundation::UInt64 FailedSends() const noexcept
    {
        return m_failedSends;
    }

private:
    struct State
    {
        SessionEvent Event{};
        Foundation::UInt64 Version{0U};
        bool Pending{false};
    };
    [[nodiscard]] Foundation::Size StateIndex(const SessionEvent& event) const noexcept;
    void Overflow(Foundation::UInt64 now) noexcept;
    void Delivered(const SessionEvent& event) noexcept;
    SessionEvent m_queue[ClientInputCapacity]{};
    State m_state[ClientStateCapacity]{};
    SessionEvent m_held[ClientHeldCapacity]{};
    Foundation::UInt64 m_close[SessionMaximumSurfaces]{};
    Foundation::UInt8 m_buttons[SessionMaximumSurfaces]{};
    Foundation::Size m_first{0U};
    Foundation::Size m_count{0U};
    Foundation::Size m_maximum{0U};
    Foundation::Size m_heldCount{0U};
    Foundation::Size m_builtCount{0U};
    Foundation::Size m_builtKeys{0U};
    Foundation::Size m_builtIndices[SessionMaximumEvents]{};
    Foundation::UInt64 m_builtVersions[SessionMaximumEvents]{};
    SessionEvent m_built[SessionMaximumEvents]{};
    Foundation::UInt64 m_dropped{0U};
    Foundation::UInt64 m_builtDropped{0U};
    Foundation::UInt64 m_noCreditSince{0U};
    Foundation::UInt64 m_retry{0U};
    Foundation::UInt64 m_failedSends{0U};
    Foundation::UInt64 m_blockedSince{0U};
    bool m_credit{false};
    bool m_notResponding{false};
    bool m_builtValid{false};
    bool m_builtMore{false};
};
} // Namespace Asteria::User::Session
#endif
