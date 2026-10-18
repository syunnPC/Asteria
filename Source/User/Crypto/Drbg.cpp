// SPDX-License-Identifier: Apache-2.0

#include "User/Crypto/Internal.hpp"

namespace Asteria::User::Crypto
{
namespace
{
using Detail::Cleanup;
using Detail::Clear;
using Detail::Copy;
using Detail::DisjointWork;
using Detail::HashUpdate;
using Detail::HmacFinish;
using Detail::HmacStart;
using Detail::Overlap;
using Detail::Read;
using Detail::Valid;
inline constexpr UInt64 ReseedInterval{1ULL << 48U};
inline constexpr Size MaximumSeedBytes{1024U};
[[nodiscard]] bool SeedInput(Workspace& w, Bytes b) noexcept
{
    return Valid(b) && b.Length <= MaximumSeedBytes && DisjointWork(w, b);
}
} // namespace
// SP 800-90A Rev.1 §10.1.2.2: concatenate provided_data without allocating it.
void HmacDrbg::Update(Workspace& w, Bytes first, Bytes second, Bytes third) noexcept
{
    const bool supplied = first.Length != 0U || second.Length != 0U || third.Length != 0U;
    for (UInt8 separator = 0U; separator < (supplied ? 2U : 1U); ++separator)
    {
        HmacStart(w, HashAlgorithm::Sha256, {m_key, 32U});
        HashUpdate(w.Hash, HashAlgorithm::Sha256, {m_value, 32U});
        HashUpdate(w.Hash, HashAlgorithm::Sha256, {&separator, 1U});
        HashUpdate(w.Hash, HashAlgorithm::Sha256, first);
        HashUpdate(w.Hash, HashAlgorithm::Sha256, second);
        HashUpdate(w.Hash, HashAlgorithm::Sha256, third);
        HmacFinish(w, HashAlgorithm::Sha256, m_key);
        HmacStart(w, HashAlgorithm::Sha256, {m_key, 32U});
        HashUpdate(w.Hash, HashAlgorithm::Sha256, {m_value, 32U});
        HmacFinish(w, HashAlgorithm::Sha256, m_value);
    }
}
Result HmacDrbg::Instantiate(Workspace& w, Bytes entropy, Bytes nonce, Bytes personalization) noexcept
{
    const Bytes self{reinterpret_cast<const UInt8*>(this), sizeof(*this)};
    if (!DisjointWork(w, self) || !DisjointWork(w, entropy) || !DisjointWork(w, nonce) ||
        !DisjointWork(w, personalization))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!SeedInput(w, entropy) || !SeedInput(w, nonce) || !SeedInput(w, personalization) || entropy.Length < 32U ||
        nonce.Length < 16U || Overlap(self, entropy) || Overlap(self, nonce) || Overlap(self, personalization))
    {
        return Result::InvalidArgument;
    }
    Clear();
    for (Size i = 0U; i < 32U; ++i)
    {
        m_value[i] = 1U;
    }
    Update(w, entropy, nonce, personalization);
    m_reseedCounter = 1U;
    return Result::Success;
}
Result HmacDrbg::Reseed(Workspace& w, Bytes entropy, Bytes additional) noexcept
{
    const Bytes self{reinterpret_cast<const UInt8*>(this), sizeof(*this)};
    if (!DisjointWork(w, self) || !DisjointWork(w, entropy) || !DisjointWork(w, additional))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!SeedInput(w, entropy) || !SeedInput(w, additional) || entropy.Length < 32U || Overlap(self, entropy) ||
        Overlap(self, additional))
    {
        return Result::InvalidArgument;
    }
    if (m_reseedCounter == 0U)
    {
        return Result::NotInitialized;
    }
    Update(w, entropy, additional, {nullptr, 0U});
    m_reseedCounter = 1U;
    return Result::Success;
}
Result HmacDrbg::Generate(Workspace& w, Bytes additional, MutableBytes output) noexcept
{
    const Bytes self{reinterpret_cast<const UInt8*>(this), sizeof(*this)};
    if (!DisjointWork(w, self) || !DisjointWork(w, additional) || !DisjointWork(w, Read(output)))
    {
        return Result::InvalidArgument;
    }
    Cleanup cleanup{w};
    if (!SeedInput(w, additional) || !Valid(output) || Overlap(self, additional) || Overlap(self, Read(output)) ||
        Overlap(additional, Read(output)))
    {
        return Result::InvalidArgument;
    }
    if (output.Length > MaximumDrbgRequest)
    {
        return Result::BufferSize;
    }
    if (m_reseedCounter == 0U)
    {
        return Result::NotInitialized;
    }
    if (m_reseedCounter > ReseedInterval)
    {
        return Result::ReseedRequired;
    }
    if (additional.Length != 0U)
    {
        Update(w, additional, {nullptr, 0U}, {nullptr, 0U});
    }
    for (Size offset = 0U; offset < output.Length; offset += 32U)
    {
        HmacStart(w, HashAlgorithm::Sha256, {m_key, 32U});
        HashUpdate(w.Hash, HashAlgorithm::Sha256, {m_value, 32U});
        HmacFinish(w, HashAlgorithm::Sha256, m_value);
        Copy(output.Data + offset, m_value, output.Length - offset < 32U ? output.Length - offset : 32U);
    }
    Update(w, additional, {nullptr, 0U}, {nullptr, 0U});
    ++m_reseedCounter;
    return Result::Success;
}
void HmacDrbg::Clear() noexcept
{
    Wipe({m_key, sizeof(m_key)});
    Wipe({m_value, sizeof(m_value)});
    m_reseedCounter = 0U;
}
} // namespace Asteria::User::Crypto
