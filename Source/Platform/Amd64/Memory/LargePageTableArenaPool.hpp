// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_LARGE_PAGE_TABLE_ARENA_POOL_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_LARGE_PAGE_TABLE_ARENA_POOL_HPP_INCLUDED
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"
namespace Asteria::Platform::Amd64::Memory
{
/// Two reserved arenas, serialized by AddressSpacePool. Failed preparation keeps its arena held;
/// only a quiesced root releases it.
class LargePageTableArenaPool final
{
public:
    static constexpr Foundation::Size Capacity{Bootstrap::LargeRootArenaCount};
    [[nodiscard]] bool HasAvailable() const noexcept
    {
        for (const bool held : m_held)
        {
            if (!held)
            {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] Foundation::Size TryAcquire() noexcept
    {
        for (Foundation::Size i{0U}; i < Capacity; ++i)
        {
            if (!m_held[i])
            {
                m_held[i] = true;
                return i;
            }
        }
        return Capacity;
    }
    [[nodiscard]] bool Release(const Foundation::Size arena) noexcept
    {
        if (arena >= Capacity || !m_held[arena])
        {
            return false;
        }
        m_held[arena] = false;
        return true;
    }

private:
    bool m_held[Capacity]{};
};
} // Namespace Asteria::Platform::Amd64::Memory
#endif
