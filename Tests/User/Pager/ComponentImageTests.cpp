// SPDX-License-Identifier: Apache-2.0
#include "User/Pager/ComponentImage.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;

void Store(UInt8* const bytes, const UInt64 value, const Size width) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

// The embedded table (ComponentImage.hpp): images found by name, whatever
// their order; an unknown name, a too-long one or a malformed table finds
// nothing.
void TestImageTable(TestContext& runner) noexcept
{
    alignas(8) UInt8 table[16U + (2U * 32U) + 24U]{};
    Store(table, 2U, 8U);
    const auto entry = [&](const Size index, const char* const name, const UInt64 offset, const UInt64 bytes) noexcept
    {
        UInt8* const record = table + 16U + (index * 32U);
        for (Size byte{0U}; name[byte] != '\0'; ++byte)
        {
            record[byte] = static_cast<UInt8>(name[byte]);
        }
        Store(record + 16U, offset, 8U);
        Store(record + 24U, bytes, 8U);
    };
    entry(0U, "directory", 80U, 16U);
    entry(1U, "component", 96U, 8U);
    const UInt8* image{nullptr};
    Size bytes{0U};
    const bool component = User::Pager::FindComponentImage(table, sizeof(table), "component", image, bytes);
    const bool componentAt = (image == (table + 96U)) && (bytes == 8U);
    const bool directory = User::Pager::FindComponentImage(table, sizeof(table), "directory", image, bytes);
    const bool directoryAt = (image == (table + 80U)) && (bytes == 16U);
    const bool unknown = User::Pager::FindComponentImage(table, sizeof(table), "client", image, bytes);
    const bool prefix = User::Pager::FindComponentImage(table, sizeof(table), "comp", image, bytes);
    const bool longer = User::Pager::FindComponentImage(table, sizeof(table), "componentx", image, bytes);
    alignas(8) UInt8 beyond[sizeof(table)]{};
    for (Size index{0U}; index < sizeof(table); ++index)
    {
        beyond[index] = table[index];
    }
    Store(beyond + 16U + 32U + 16U, 200U, 8U);
    const bool outside = User::Pager::FindComponentImage(beyond, sizeof(beyond), "component", image, bytes);
    Store(beyond, User::Pager::MaximumComponentImages + 1U, 8U);
    const bool tooMany = User::Pager::FindComponentImage(beyond, sizeof(beyond), "directory", image, bytes);
    ASTERIA_EXPECT(runner, "the embedded table yields each image by its exact name and refuses a malformed one",
                   component, componentAt, directory, directoryAt, !unknown, !prefix, !longer, !outside, !tooMany);

    // The sixteenth module reaches the root service.
    constexpr Size Count{User::Pager::MaximumComponentImages};
    constexpr Size Images{16U + (Count * 32U)};
    static UInt8 full[Images + Count]{};
    Store(full, Count, 8U);
    for (Size index{0U}; index < Count; ++index)
    {
        UInt8* const record = full + 16U + (index * 32U);
        record[0U] = static_cast<UInt8>('a' + index);
        Store(record + 16U, Images + index, 8U);
        Store(record + 24U, 1U, 8U);
    }
    ASTERIA_EXPECT(runner, "the complete sixteen-entry module table is accepted", Count == 16U,
                   User::Pager::FindComponentImage(full, sizeof(full), "p", image, bytes),
                   image == full + Images + Count - 1U, bytes == 1U);
}

// The rules a component is described under: the ISA's range and entry, the
// root service's three segments of twenty-four pages, no data with file bytes.
void TestComponentRules(TestContext& runner) noexcept
{
    constexpr auto rules = User::Pager::MakeComponentRules(User::Loader::ExecutableMachine::Ia64, 0x2000U, 0x9000U,
                                                           true, User::Pager::Ia64ComponentSegmentPages);
    ASTERIA_EXPECT(runner, "a component's rules carry the ISA's range and the root service's limits",
                   rules.Machine == User::Loader::ExecutableMachine::Ia64, rules.UserStart == 0x2000U,
                   rules.UserLimit == 0x9000U, rules.EntryDescriptor, rules.MaximumSegments == 3U,
                   rules.MaximumSegmentPages == 128U, !rules.WritableFileBytes);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ComponentImageTests"};
    Asteria::Test::TestImageTable(runner);
    Asteria::Test::TestComponentRules(runner);
    return runner.Finish();
}
