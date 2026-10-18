// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ComponentImage.hpp"

namespace Asteria::User::Pager
{
namespace
{
[[nodiscard]] Foundation::UInt64 Load(const Foundation::UInt8* const bytes, const Foundation::Size width) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < width; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}
} // namespace

bool FindComponentImage(const Foundation::UInt8* const table, const Foundation::Size tableBytes, const char* const name,
                        const Foundation::UInt8*& image, Foundation::Size& imageBytes) noexcept
{
    constexpr Foundation::Size TableHeaderBytes{16U};
    constexpr Foundation::Size EntryBytes{ComponentImageNameBytes + 16U};
    if ((table == nullptr) || (tableBytes < TableHeaderBytes))
    {
        return false;
    }
    const Foundation::UInt64 count = Load(table, 8U);
    // No multiplication: entries are 32 bytes.
    if ((count > MaximumComponentImages) || ((count << 5U) > (tableBytes - TableHeaderBytes)))
    {
        return false;
    }
    static_assert(EntryBytes == 32U);
    for (Foundation::UInt64 index{0U}; index < count; ++index)
    {
        const Foundation::UInt8* const entry = table + TableHeaderBytes + (index << 5U);
        bool same{true};
        bool ended{false};
        for (Foundation::Size byte{0U}; byte < ComponentImageNameBytes; ++byte)
        {
            const char wanted = ended ? '\0' : name[byte];
            ended = ended || (wanted == '\0');
            same = same && (static_cast<char>(entry[byte]) == wanted);
        }
        const Foundation::UInt64 offset = Load(entry + ComponentImageNameBytes, 8U);
        const Foundation::UInt64 bytes = Load(entry + ComponentImageNameBytes + 8U, 8U);
        if (!same || !ended)
        {
            continue;
        }
        if ((offset > tableBytes) || (bytes > (tableBytes - offset)))
        {
            return false;
        }
        image = table + offset;
        imageBytes = bytes;
        return true;
    }
    return false;
}
} // namespace Asteria::User::Pager
