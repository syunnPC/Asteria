// SPDX-License-Identifier: Apache-2.0
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64TranslationCachePurge.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Memory::Ia64LocalTranslationCachePurge;
using Platform::Ia64::Memory::MaximumTranslationCachePurgeOperations;
using Platform::Ia64::Memory::PalTranslationCachePurgePlan;

// Two outer iterations of three purges: base, +0x100 and +0x200, then, after
// the third inner stride and the outer stride, +0x1300, +0x1400 and +0x1500.
[[nodiscard]] constexpr PalTranslationCachePurgePlan Plan() noexcept
{
    return PalTranslationCachePurgePlan{
        .BaseAddress = 0x8000'0000U,
        .OperationCount = 6U,
        .LastPurgeAddress = 0x8000'1500U,
        .OuterCount = 2U,
        .InnerCount = 3U,
        .OuterStride = 0x1000U,
        .InnerStride = 0x100U,
    };
}

void TestConfiguration(TestContext& runner) noexcept
{
    PalTranslationCachePurgePlan empty = Plan();
    empty.InnerCount = 0U;
    PalTranslationCachePurgePlan mismatched = Plan();
    mismatched.OperationCount = 5U;
    PalTranslationCachePurgePlan oversized = Plan();
    oversized.OuterCount = 65'537U;
    oversized.InnerCount = 1U;
    oversized.OperationCount = 65'537U;
    Ia64LocalTranslationCachePurge purge;
    ASTERIA_EXPECT(
        runner, "a purge is configured once, for one valid processor, with a loop the decoder could produce",
        !purge.CanPurge(CpuId{0U}), !purge.Configure(CpuId{}, Plan()), !purge.Configure(CpuId{0U}, empty),
        !purge.Configure(CpuId{0U}, mismatched), oversized.OperationCount > MaximumTranslationCachePurgeOperations,
        !purge.Configure(CpuId{0U}, oversized), purge.Configure(CpuId{0U}, Plan()), !purge.Configure(CpuId{0U}, Plan()),
        purge.CanPurge(CpuId{0U}), !purge.CanPurge(CpuId{1U}), !purge.CanPurge(CpuId{}));
}

void TestPurge(TestContext& runner) noexcept
{
    Ia64LocalTranslationCachePurge purge;
    const bool configured = purge.Configure(CpuId{0U}, Plan());
    purge.Purge(CpuId{1U});
    const auto refused = purge.GetSnapshot();
    purge.Purge(CpuId{0U});
    const auto once = purge.GetSnapshot();
    purge.Purge(CpuId{0U});
    const auto twice = purge.GetSnapshot();
    ASTERIA_EXPECT(runner,
                   "each purge runs the whole PAL loop on its own processor only and ends at the loop's last address",
                   configured, refused.Purges == 0U, refused.Operations == 0U, once.Purges == 1U,
                   once.Operations == Plan().OperationCount, once.LastAddress == Plan().LastPurgeAddress,
                   twice.Purges == 2U, twice.Operations == (2U * Plan().OperationCount));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64TranslationCachePurgeTests"};
    Asteria::Test::TestConfiguration(runner);
    Asteria::Test::TestPurge(runner);
    return runner.Finish();
}
