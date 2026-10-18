// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_INPUT_ORDER_HPP_INCLUDED
#define ASTERIA_USER_SESSION_INPUT_ORDER_HPP_INCLUDED
#include "User/Session/SessionFrame.hpp"
namespace Asteria::User::Session
{
/// Each queue preserves its broker stream order. Caller consumes the returned action immediately;
/// no focus is saved while waiting. Reset or a five-second wait drops BOTH streams and counts them.
/// Fixed 64 entry streams are bounded independently of downstream credit.
class InputOrder final
{
public:
    static constexpr Foundation::Size Capacity{64U};
    enum class Kind : Foundation::UInt8
    {
        None,
        Keys,
        Pointer
    };
    struct Item
    {
        Kind Type;
        Foundation::UInt64 Dependency;
        Foundation::UInt64 Count;
        Foundation::UInt64 Token;
        Foundation::UInt64 Since;
    };
    void Configure(Foundation::UInt64 inputEpoch, Foundation::UInt64 faceEpoch) noexcept;
    /// A fresh receiver begins at the broker's immutable rebind fence;
    /// pending keys remain counted in the original registration.
    void Resume(Foundation::UInt64 keys, Foundation::UInt64 pointer) noexcept;
    [[nodiscard]] bool Keys(Foundation::UInt64 horizon, Foundation::UInt64 count, Foundation::UInt64 token,
                            Foundation::UInt64 now) noexcept;
    [[nodiscard]] bool Pointer(Foundation::UInt64 sequence, Foundation::UInt64 stamp, Foundation::UInt64 token,
                               Foundation::UInt64 now) noexcept;
    [[nodiscard]] Item Take(Foundation::UInt64 now) noexcept;
    [[nodiscard]] Foundation::UInt64 AppliedKeys() const noexcept
    {
        return m_keys;
    }
    [[nodiscard]] Foundation::UInt64 AppliedPointer() const noexcept
    {
        return m_pointer;
    }
    [[nodiscard]] Foundation::UInt64 Dropped() const noexcept
    {
        return m_dropped;
    }

private:
    void Drop() noexcept;
    static_assert((Capacity & (Capacity - 1U)) == 0U);
    Item m_keyQueue[Capacity]{};
    Item m_pointerQueue[Capacity]{};
    Foundation::Size m_keyFirst{0U};
    Foundation::Size m_keyCount{0U};
    Foundation::Size m_pointerFirst{0U};
    Foundation::Size m_pointerCount{0U};
    Foundation::UInt64 m_inputEpoch{0U};
    Foundation::UInt64 m_faceEpoch{0U};
    Foundation::UInt64 m_keys{0U};
    Foundation::UInt64 m_pointer{0U};
    Foundation::UInt64 m_dropped{0U};
    Foundation::UInt64 m_lastSequence{0U};
    bool m_stale{false};
};
} // Namespace Asteria::User::Session
#endif
