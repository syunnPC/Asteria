// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Device/Amd64MmioWindow.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Device::FindAmd64MmioWindowLeaf;
alignas(4096U) UInt64 g_tables[4U][512U]{};
} // namespace
int main()
{
    TestContext runner{"Amd64MmioWindowTests"};
    const UInt64 root = reinterpret_cast<Asteria::Foundation::UIntPointer>(g_tables);
    g_tables[0U][0U] = (root + 4096U) | 3U;
    g_tables[1U][1U] = (root + 8192U) | 3U;
    g_tables[2U][384U] = (root + 12288U) | 3U;
    g_tables[3U][0U] = 0x8000'0000'FEE0'001BULL;
    UInt64* selected{nullptr};
    ASTERIA_EXPECT(runner, "owned UC/NX aperture", FindAmd64MmioWindowLeaf(root, 4U, selected),
                   selected == &g_tables[3U][0U]);
    for (unsigned mutation{0U}; mutation < 7U; ++mutation)
    {
        constexpr UInt64 rejected[]{0xFEE0001BU,           0x80000000FEE0001FULL, 0x80000000FEE0011BULL,
                                    0x80000000FEE00013ULL, 0x80000000FEE00019ULL, 0x80000000FEE0001AULL,
                                    0x80000000FEE0009BULL};
        g_tables[3U][0U] = rejected[mutation];
        ASTERIA_EXPECT(runner, "reject executable/user/global/cache/readonly/absent/PAT leaves",
                       !FindAmd64MmioWindowLeaf(root, 4U, selected));
        ASTERIA_EXPECT(runner, "failure preserves output", selected == &g_tables[3U][0U]);
    }
    g_tables[3U][0U] = 0x8000'0000'FEE0'001BULL;
    g_tables[1U][1U] = (root + 4U * 4096U) | 3U;
    ASTERIA_EXPECT(runner, "parent outside arena", !FindAmd64MmioWindowLeaf(root, 4U, selected));
    g_tables[1U][1U] = (root + 8192U) | 0x83U;
    ASTERIA_EXPECT(runner, "large parent", !FindAmd64MmioWindowLeaf(root, 4U, selected));
    ASTERIA_EXPECT(runner, "root geometry", !FindAmd64MmioWindowLeaf(root + 1U, 4U, selected),
                   !FindAmd64MmioWindowLeaf(root, 3U, selected), !FindAmd64MmioWindowLeaf(root, 65U, selected));
    return runner.Finish();
}
