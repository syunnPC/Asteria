// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"

#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test
{
void TestScanoutAttributes(TestSupport::TestContext& context) noexcept
{
    using Kernel::Memory::MemoryCachePolicy;
    constexpr MemoryCachePolicy policies[]{MemoryCachePolicy::WriteCombining, MemoryCachePolicy::Uncached};
    for (const MemoryCachePolicy policy : policies)
    {
        const Foundation::UInt64 entry = Platform::Amd64::Diagnostics::Amd64StopScanoutAttributes(policy);
        const Foundation::UInt64 index = ((entry >> 3U) & 3U) | ((entry >> 5U) & 4U);
        const Foundation::UInt64 memoryType =
            (Platform::Amd64::Memory::ExpectedInitialPatValue >> (index * 8U)) & 0xFFU;
        ASTERIA_EXPECT(context, "stop aperture matches WC or UC publication with supervisor RW/NX protection",
                       (entry & 7U) == 3U, (entry >> 63U) == 1U,
                       memoryType == ((policy == MemoryCachePolicy::WriteCombining) ? 1U : 0U));
    }
    ASTERIA_EXPECT(context, "stop scanout refuses cached and unknown policies",
                   Platform::Amd64::Diagnostics::Amd64StopScanoutAttributes(MemoryCachePolicy::WriteBack) == 0U,
                   Platform::Amd64::Diagnostics::Amd64StopScanoutAttributes(MemoryCachePolicy::WriteThrough) == 0U,
                   Platform::Amd64::Diagnostics::Amd64StopScanoutAttributes(static_cast<MemoryCachePolicy>(0xFFU)) ==
                       0U);
}
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"Amd64StopTests"};
    Asteria::Test::TestScanoutAttributes(context);
    return context.Finish();
}
