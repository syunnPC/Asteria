// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/TrustedScreen.hpp"
namespace Asteria::User::Identity
{
bool SecretEntry::Append(const TextView bytes) noexcept
{
    if (bytes.Length == 0U || bytes.Bytes == nullptr || bytes.Length > MaximumPasswordBytes - m_length ||
        !Authentication::ValidPassword(bytes))
    {
        return false;
    }
    for (Size i{0U}; i < bytes.Length; ++i)
    {
        m_bytes[m_length++] = bytes.Bytes[i];
    }
    return true;
}
void SecretEntry::Backspace() noexcept
{
    if (m_length == 0U)
    {
        return;
    }
    Size first = m_length - 1U;
    while (first != 0U && (m_bytes[first] & 0xC0U) == 0x80U)
    {
        --first;
    }
    Crypto::Wipe({m_bytes + first, m_length - first});
    m_length = first;
}
void SecretEntry::Clear() noexcept
{
    Crypto::Wipe({m_bytes, sizeof(m_bytes)});
    m_length = 0U;
}
Size SecretEntry::Characters() const noexcept
{
    Size count{0U};
    for (Size i{0U}; i < m_length; ++i)
    {
        count += (m_bytes[i] & 0xC0U) != 0x80U ? 1U : 0U;
    }
    return count;
}
} // namespace Asteria::User::Identity
