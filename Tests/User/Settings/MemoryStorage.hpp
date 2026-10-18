// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_SETTINGS_MEMORY_STORAGE_HPP_INCLUDED
#define ASTERIA_TESTS_USER_SETTINGS_MEMORY_STORAGE_HPP_INCLUDED
#include "User/Settings/Database.hpp"
#include <cstring>
namespace Asteria::TestSupport::Settings
{
namespace S = User::Settings;
struct MemoryStorage
{
    Foundation::UInt8 Bytes[S::StorageBytes]{};
    Foundation::Size Readable{S::StorageBytes};
    Foundation::Size Calls{0U};
    Foundation::Size FailCall{0U};
    Foundation::Size Prefix{0U};
    bool CorruptWrite{false};
    Foundation::Size Offsets[6U]{};
    Foundation::Size Lengths[6U]{};
    Foundation::UInt8 Boundaries[6U][S::StorageBytes]{};
    bool Capture{false};
    [[nodiscard]] S::Storage Interface() noexcept
    {
        return {this, S::StorageBytes, Read, Write, Flush};
    }
    static bool Read(void* context, Foundation::UInt64 offset, Foundation::UInt8* out, Foundation::Size count) noexcept
    {
        auto& self = *static_cast<MemoryStorage*>(context);
        if ((offset > self.Readable) || (count > self.Readable - offset))
        {
            return false;
        }
        std::memcpy(out, self.Bytes + offset, count);
        return true;
    }
    static bool Write(void* context, Foundation::UInt64 offset, const Foundation::UInt8* in,
                      Foundation::Size count) noexcept
    {
        auto& self = *static_cast<MemoryStorage*>(context);
        ++self.Calls;
        if ((offset > S::StorageBytes) || (count > S::StorageBytes - offset))
        {
            return false;
        }
        const bool fail = self.Calls == self.FailCall;
        const auto copied = fail ? (self.Prefix < count ? self.Prefix : count) : count;
        std::memcpy(self.Bytes + offset, in, copied);
        if (fail && self.CorruptWrite && (copied != 0U))
        {
            self.Bytes[offset + copied - 1U] ^= 0x80U;
        }
        self.Record(static_cast<Foundation::Size>(offset), count);
        return !fail;
    }
    static bool Flush(void* context) noexcept
    {
        auto& self = *static_cast<MemoryStorage*>(context);
        ++self.Calls;
        self.Record(0U, 0U);
        return self.Calls != self.FailCall;
    }
    void Record(Foundation::Size offset, Foundation::Size length) noexcept
    {
        if (Capture && (Calls <= 6U))
        {
            Offsets[Calls - 1U] = offset;
            Lengths[Calls - 1U] = length;
            std::memcpy(Boundaries[Calls - 1U], Bytes, S::StorageBytes);
        }
    }
    void Reset() noexcept
    {
        std::memset(Bytes, 0, sizeof(Bytes));
        Readable = S::StorageBytes;
        Calls = 0U;
        FailCall = 0U;
        Prefix = 0U;
        CorruptWrite = false;
        Capture = false;
    }
};
[[nodiscard]] inline S::TextView View(const char* text) noexcept
{
    return {reinterpret_cast<const Foundation::UInt8*>(text), std::strlen(text)};
}
inline constexpr S::Access Owner{1U, S::AllRights};
inline constexpr S::AclEntry RootAcl{1U, S::AllRights, false, true};
[[nodiscard]] inline bool Equal(const S::Value& a, const S::Value& b) noexcept
{
    return (a.Type == b.Type) && (a.Length == b.Length) && (std::memcmp(a.Bytes, b.Bytes, a.Length) == 0);
}
} // namespace Asteria::TestSupport::Settings
#endif
