// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Amd64/Memory/Amd64LocalTlbInvalidator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::UInt64 g_invalidatedPages[512]{};
Foundation::Size g_invalidationCount{0U};
} // namespace Asteria::Test

extern "C" void AsteriaAmd64InvalidatePage(const Asteria::Foundation::UInt64 virtualAddress) noexcept
{
    if (Asteria::Test::g_invalidationCount < 512U)
    {
        Asteria::Test::g_invalidatedPages[Asteria::Test::g_invalidationCount] = virtualAddress;
    }
    ++Asteria::Test::g_invalidationCount;
}

namespace Asteria::Test
{
using Hal::Memory::ILocalTlbInvalidator;
using Hal::Memory::LocalTlbInvalidationError;
using Kernel::Memory::PageCount;
using Kernel::Memory::VirtualAddress;
using Platform::Amd64::Memory::Amd64LocalTlbInvalidator;

struct InvalidRangeCase
{
    VirtualAddress Start;
    PageCount Pages;
    LocalTlbInvalidationError Error;
};

void ResetLog() noexcept
{
    for (Foundation::UInt64& address : g_invalidatedPages)
    {
        address = 0U;
    }
    g_invalidationCount = 0U;
}

void TestValidation(TestContext& runner) noexcept
{
    Amd64LocalTlbInvalidator invalidator;
    ILocalTlbInvalidator& interface = invalidator;
    constexpr InvalidRangeCase Cases[]{
        {VirtualAddress{0x1001U}, PageCount{1U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0x1000U}, PageCount{0U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0x1000U}, PageCount{513U}, LocalTlbInvalidationError::RangeTooLarge},
        {VirtualAddress{0xFFFF'FFFF'FFFF'F000ULL}, PageCount{2U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0x0000'8000'0000'0000ULL}, PageCount{1U}, LocalTlbInvalidationError::InvalidRange},
    };
    for (const InvalidRangeCase& test : Cases)
    {
        ResetLog();
        ASTERIA_EXPECT(runner, "invalid local invalidation range performs no native operation",
                       interface.Invalidate(test.Start, test.Pages) == test.Error, g_invalidationCount == 0U);
    }
}

void TestExactInvalidations(TestContext& runner) noexcept
{
    Amd64LocalTlbInvalidator invalidator;
    ResetLog();
    ASTERIA_EXPECT(runner, "lower-canonical range invalidates every page exactly once",
                   invalidator.Invalidate(VirtualAddress{0x0000'0000'0040'0000ULL}, PageCount{3U}) ==
                       LocalTlbInvalidationError::None,
                   g_invalidationCount == 3U, g_invalidatedPages[0U] == 0x0040'0000U,
                   g_invalidatedPages[1U] == 0x0040'1000U, g_invalidatedPages[2U] == 0x0040'2000U);

    ResetLog();
    ASTERIA_EXPECT(runner, "maximum upper-canonical range remains bounded and complete",
                   invalidator.Invalidate(VirtualAddress{0xFFFF'8000'0000'0000ULL}, PageCount{512U}) ==
                       LocalTlbInvalidationError::None,
                   g_invalidationCount == 512U, g_invalidatedPages[0U] == 0xFFFF'8000'0000'0000ULL,
                   g_invalidatedPages[511U] == 0xFFFF'8000'001F'F000ULL);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64LocalTlbInvalidatorTests"};
    Asteria::Test::TestValidation(runner);
    Asteria::Test::TestExactInvalidations(runner);
    return runner.Finish();
}
