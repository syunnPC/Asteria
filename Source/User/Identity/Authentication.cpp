// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/Internal.hpp"
namespace Asteria::User::Identity
{
namespace
{
[[nodiscard]] UInt32 Select(UInt32 bit, UInt32 yes, UInt32 no) noexcept
{
    volatile UInt32 opaque = 0U - bit;
    const UInt32 mask = opaque;
    opaque = 0U;
    return (yes & mask) | (no & ~mask);
}
[[nodiscard]] bool Cancelled(Cancellation c) noexcept
{
    return c.Requested != nullptr && c.Requested(c.Context);
}
} // Namespace
// Unicode 16.0 §3.9 Table 3-7. Fixed public indices, complete scan, no
// secret-conditioned branches, lookup or early refusal.
bool Authentication::ValidPassword(TextView password) noexcept
{
    if (password.Bytes == nullptr || password.Length == 0U || password.Length > MaximumPasswordBytes)
    {
        return false;
    }
    UInt32 state[4U]{0U, 0x80U, 0xBFU, 0U}; // Remaining, lower, upper, accumulated invalid
    for (Size i{0U}; i < password.Length; ++i)
    {
        const UInt32 b = password.Bytes[i];
        const UInt32 continuation = static_cast<UInt32>(state[0U] != 0U);
        const UInt32 ascii = static_cast<UInt32>(b < 0x80U);
        const UInt32 two = static_cast<UInt32>(b >= 0xC2U) & static_cast<UInt32>(b <= 0xDFU);
        const UInt32 three = static_cast<UInt32>(b >= 0xE0U) & static_cast<UInt32>(b <= 0xEFU);
        const UInt32 four = static_cast<UInt32>(b >= 0xF0U) & static_cast<UInt32>(b <= 0xF4U);
        const UInt32 valid =
            Select(continuation, static_cast<UInt32>(b >= state[1U]) & static_cast<UInt32>(b <= state[2U]),
                   ascii | two | three | four);
        state[3U] |= (valid ^ 1U) | static_cast<UInt32>(b == 0U);
        state[0U] = Select(continuation, state[0U] - 1U, two + 2U * three + 3U * four);
        const UInt32 lower =
            Select(static_cast<UInt32>(b == 0xE0U), 0xA0U, Select(static_cast<UInt32>(b == 0xF0U), 0x90U, 0x80U));
        const UInt32 upper =
            Select(static_cast<UInt32>(b == 0xEDU), 0x9FU, Select(static_cast<UInt32>(b == 0xF4U), 0x8FU, 0xBFU));
        state[1U] = Select(continuation, 0x80U, lower);
        state[2U] = Select(continuation, 0xBFU, upper);
    }
    const bool valid = (state[0U] | state[3U]) == 0U;
    Detail::Clear(state, sizeof(state));
    return valid;
}
bool Authentication::Buffers(TextView password, const void* output, Size outputBytes) const noexcept
{
    if (m_arena == nullptr || m_blocks < ArenaBlocks || output == nullptr ||
        (reinterpret_cast<UInt64>(m_arena) % alignof(Crypto::Argon2Block)) != 0U ||
        password.Length > MaximumPasswordBytes || (password.Length != 0U && password.Bytes == nullptr))
    {
        return false;
    }
    return !Detail::Overlap(&m_work, sizeof(m_work), m_arena, ArenaBytes) &&
           !Detail::Overlap(&m_work, sizeof(m_work), password.Bytes, password.Length) &&
           !Detail::Overlap(&m_work, sizeof(m_work), output, outputBytes) &&
           !Detail::Overlap(m_arena, ArenaBytes, password.Bytes, password.Length) &&
           !Detail::Overlap(m_arena, ArenaBytes, output, outputBytes) &&
           !Detail::Overlap(password.Bytes, password.Length, output, outputBytes);
}
void Authentication::Clear() noexcept
{
    Detail::Clear(&m_work, sizeof(m_work));
    Detail::Clear(m_arena, ArenaBytes);
}
Status Authentication::Hash(Size length, Cancellation cancel) noexcept
{
    if (Cancelled(cancel))
    {
        return Status::Cancelled;
    }
    const auto result =
        Crypto::Argon2id(m_work.CryptoWork, Crypto::LoginArgon2Defaults, {m_work.Password, length},
                         {m_work.Stored.Salt, Crypto::LoginArgon2SaltBytes}, {nullptr, 0U}, {nullptr, 0U}, m_arena,
                         ArenaBlocks, {m_work.Tag, Crypto::LoginArgon2TagBytes}, {cancel.Context, cancel.Poll});
    if (result != Crypto::Result::Success)
    {
        return Status::Unavailable;
    }
    return Cancelled(cancel) ? Status::Cancelled : Status::Ok;
}
Status Authentication::Enroll(TextView password, Entropy entropy, Credential& out, Cancellation cancel) noexcept
{
    if (!Buffers(password, &out, sizeof(out)))
    {
        return Status::Unavailable;
    }
    if (entropy.Fill == nullptr)
    {
        Clear();
        return Status::Unavailable;
    }
    Detail::Clear(&m_work, sizeof(m_work));
    // Refuse malformed selection before requesting a fresh trusted salt.
    Detail::Copy(m_work.Password, password.Bytes, password.Length);
    if (!ValidPassword({m_work.Password, password.Length}))
    {
        Clear();
        return Status::Invalid;
    }
    if (!entropy.Fill(entropy.Context, m_work.Stored.Salt, Crypto::LoginArgon2SaltBytes))
    {
        Clear();
        return Status::Unavailable;
    }
    const auto result = Hash(password.Length, cancel);
    if (result == Status::Ok)
    {
        m_work.Stored.Profile = 1U;
        Detail::Copy(m_work.Stored.Tag, m_work.Tag, Crypto::LoginArgon2TagBytes);
        out = m_work.Stored;
    }
    Clear();
    return result;
}
Status Authentication::Enroll(AccountStore& store, PrincipalId id, TextView password, Entropy entropy,
                              Cancellation cancel) noexcept
{
    if (Detail::Overlap(&store, sizeof(store), &m_work, sizeof(m_work)) ||
        Detail::Overlap(&store, sizeof(store), m_arena, ArenaBytes))
    {
        return Status::Invalid;
    }
    if (!store.m_open)
    {
        return Status::Closed;
    }
    const auto index = store.AccountIndex(id);
    if (index == AccountCapacity)
    {
        return Status::NotFound;
    }
    if (store.m_state.Accounts[index].Info.Revision == MaximumCounter || store.m_generation == MaximumCounter)
    {
        return Status::Exhausted;
    }
    Credential credential{};
    const auto result = Enroll(password, entropy, credential, cancel);
    const auto published = result == Status::Ok ? store.SetCredential(id, credential) : result;
    Detail::Clear(&credential, sizeof(credential));
    return published;
}
Status Authentication::Verify(AccountStore& store, AttemptLimiter& limiter, TextView name, TextView password,
                              UInt64 session, UInt64 operation, PrincipalContext& out, UInt64& retryAt,
                              Cancellation cancel) noexcept
{
    if (!Buffers(password, &out, sizeof(out)) || Detail::Overlap(&store, sizeof(store), &m_work, sizeof(m_work)) ||
        Detail::Overlap(&store, sizeof(store), m_arena, ArenaBytes) ||
        Detail::Overlap(name.Bytes, name.Length, &m_work, sizeof(m_work)) ||
        Detail::Overlap(name.Bytes, name.Length, m_arena, ArenaBytes) ||
        Detail::Overlap(&retryAt, sizeof(retryAt), &m_work, sizeof(m_work)) ||
        Detail::Overlap(&retryAt, sizeof(retryAt), m_arena, ArenaBytes) ||
        Detail::Overlap(&retryAt, sizeof(retryAt), &out, sizeof(out)) ||
        Detail::Overlap(&store, sizeof(store), &out, sizeof(out)) ||
        Detail::Overlap(&store, sizeof(store), &retryAt, sizeof(retryAt)) ||
        Detail::Overlap(&limiter.m_state, sizeof(limiter.m_state), &m_work, sizeof(m_work)) ||
        Detail::Overlap(&limiter.m_state, sizeof(limiter.m_state), m_arena, ArenaBytes) ||
        Detail::Overlap(&limiter.m_state, sizeof(limiter.m_state), &out, sizeof(out)) ||
        Detail::Overlap(&limiter.m_state, sizeof(limiter.m_state), &retryAt, sizeof(retryAt)))
    {
        return Status::Unavailable;
    }
    if (!store.m_open)
    {
        return Status::Closed;
    }
    // An unprovisioned store cannot issue a principal.
    if (store.Ready() != Status::Ok)
    {
        Clear();
        return Status::NotProvisioned;
    }
    if (!FormatCodec::ValidName(name) || session == 0U)
    {
        Clear();
        return Status::Invalid;
    }
    Detail::Clear(&m_work, sizeof(m_work));
    Detail::Copy(m_work.Password, password.Bytes, password.Length);
    if (!ValidPassword({m_work.Password, password.Length}))
    {
        Clear();
        return Status::Invalid;
    }
    const auto found = store.FindAccount(name, m_work.Info);
    const bool known = found == Status::Ok;
    UInt64 serial{0U};
    const auto admitted = limiter.Begin(known ? m_work.Info.Id : 0U, serial, retryAt);
    if (admitted != Status::Ok)
    {
        Clear();
        return admitted;
    }
    if (known && !m_work.Info.Disabled)
    {
        m_work.Stored = store.m_state.Accounts[store.AccountIndex(m_work.Info.Id)].Password;
    }
    // Unknown/disabled/unprovisioned records still hash the fixed profile.
    // All-zero synthetic dummy salt/tag is private and conveys no account.
    const auto result = Hash(password.Length, cancel);
    const bool equal =
        Crypto::Equal({m_work.Tag, Crypto::LoginArgon2TagBytes}, {m_work.Stored.Tag, Crypto::LoginArgon2TagBytes});
    bool success = result == Status::Ok && equal && known && !m_work.Info.Disabled && m_work.Stored.Profile == 1U;
    if (success && store.BuildContext(m_work.Info.Id, session, operation, m_work.Context) != Status::Ok)
    {
        success = false;
    }
    const auto completed = limiter.Complete(serial, success, result == Status::Cancelled);
    if (completed == Status::Ok && success)
    {
        out = m_work.Context;
    }
    Clear();
    if (completed != Status::Ok)
    {
        return completed;
    }
    if (result == Status::Cancelled)
    {
        return Status::Cancelled;
    }
    return success ? Status::Ok : Status::Failed;
}
} // Namespace Asteria::User::Identity
