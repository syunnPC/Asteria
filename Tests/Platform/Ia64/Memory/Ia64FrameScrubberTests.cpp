// SPDX-License-Identifier: Apache-2.0
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64FrameScrubber.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::FrameScrubError;
using Kernel::Memory::PhysicalAddress;
using Platform::Ia64::Memory::Ia64FrameScrubber;

constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 PoolPages{4U};

// The hosted scrubber writes host memory, so this page-aligned buffer stands
// in for the physical frame pool, with one guard page on each side.
alignas(4096) constinit Foundation::UInt8 g_memory[(PoolPages + 2U) * PageBytes]{};

[[nodiscard]] Foundation::UInt64 PoolStart() noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_memory)) + PageBytes;
}

void Fill() noexcept
{
    for (Foundation::UInt8& byte : g_memory)
    {
        byte = 0xA5U;
    }
}

[[nodiscard]] bool PagesHold(const Foundation::UInt64 firstPage, const Foundation::UInt64 pages,
                             const Foundation::UInt8 value) noexcept
{
    for (Foundation::UInt64 index{firstPage * PageBytes}; index < ((firstPage + pages) * PageBytes); ++index)
    {
        if (g_memory[index] != value)
        {
            return false;
        }
    }
    return true;
}

void TestConfiguration(TestContext& runner) noexcept
{
    Ia64FrameScrubber scrubber;
    Fill();
    ASTERIA_EXPECT(runner, "an unconfigured scrubber faults and writes nothing",
                   scrubber.Zero(PhysicalAddress{PoolStart()}, 1U) == FrameScrubError::Faulted,
                   PagesHold(0U, PoolPages + 2U, 0xA5U), scrubber.GetSnapshot().Faults == 1U);
    ASTERIA_EXPECT(runner, "an empty, misaligned or uncacheable pool is refused", !scrubber.Configure(PoolStart(), 0U),
                   !scrubber.Configure(PoolStart() + 8U, PoolPages),
                   !scrubber.Configure(Foundation::UInt64{1U} << 63U, 1U),
                   !scrubber.Configure((Foundation::UInt64{1U} << 63U) - PageBytes, 2U));
    ASTERIA_EXPECT(runner, "the pool is configured once", scrubber.Configure(PoolStart(), PoolPages),
                   !scrubber.Configure(PoolStart(), PoolPages));
}

void TestZeroing(TestContext& runner) noexcept
{
    Ia64FrameScrubber scrubber;
    Fill();
    ASTERIA_EXPECT(runner, "the zeroing pool is configured", scrubber.Configure(PoolStart(), PoolPages));
    ASTERIA_EXPECT(runner, "a run inside the pool is zeroed exactly",
                   scrubber.Zero(PhysicalAddress{PoolStart() + PageBytes}, 2U) == FrameScrubError::None,
                   PagesHold(0U, 2U, 0xA5U), PagesHold(2U, 2U, 0U), PagesHold(4U, 2U, 0xA5U),
                   scrubber.GetSnapshot().ZeroedPages == 2U);
    ASTERIA_EXPECT(runner, "the pool's last page is zeroed",
                   scrubber.Zero(PhysicalAddress{PoolStart() + ((PoolPages - 1U) * PageBytes)}, 1U) ==
                       FrameScrubError::None,
                   PagesHold(4U, 1U, 0U), PagesHold(5U, 1U, 0xA5U));

    Fill();
    ASTERIA_EXPECT(runner, "a run leaving the pool, misaligned or empty faults and writes nothing",
                   scrubber.Zero(PhysicalAddress{PoolStart() - PageBytes}, 1U) == FrameScrubError::Faulted,
                   scrubber.Zero(PhysicalAddress{PoolStart() + (3U * PageBytes)}, 2U) == FrameScrubError::Faulted,
                   scrubber.Zero(PhysicalAddress{PoolStart() + (PoolPages * PageBytes)}, 1U) ==
                       FrameScrubError::Faulted,
                   scrubber.Zero(PhysicalAddress{PoolStart() + 8U}, 1U) == FrameScrubError::Faulted,
                   scrubber.Zero(PhysicalAddress{PoolStart()}, 0U) == FrameScrubError::Faulted,
                   scrubber.Zero(PhysicalAddress{PoolStart()}, ~Foundation::UInt64{0U}) == FrameScrubError::Faulted,
                   PagesHold(0U, PoolPages + 2U, 0xA5U), scrubber.GetSnapshot().Faults == 6U,
                   scrubber.GetSnapshot().ZeroedPages == 3U);
}
// Source/User/System/AhciPort.hpp evidence: a watched page keeps its first words as the scrub of a
// run holding it found them, once per watch; pages outside the run, and a refused run, are not
// sampled.
void TestSampling(TestContext& runner) noexcept
{
    using Platform::Ia64::Memory::Ia64FrameScrubberSampleWords;
    Ia64FrameScrubber scrubber;
    Fill();
    ASTERIA_EXPECT(runner, "the sampling pool is configured", scrubber.Configure(PoolStart(), PoolPages));
    const Foundation::UInt64 watched = PoolStart() + (2U * PageBytes);
    for (Foundation::Size index{0U}; index < (Ia64FrameScrubberSampleWords * 8U); ++index)
    {
        g_memory[(3U * PageBytes) + index] = static_cast<Foundation::UInt8>(index + 1U);
    }
    scrubber.Watch(1U, watched);
    scrubber.Watch(0U, PoolStart());
    ASTERIA_EXPECT(runner, "a refused run and a run without the watched page sample nothing",
                   scrubber.Zero(PhysicalAddress{watched}, 3U) == FrameScrubError::Faulted,
                   scrubber.Zero(PhysicalAddress{PoolStart() + PageBytes}, 1U) == FrameScrubError::None,
                   scrubber.GetSample(0U).Sampled == 0U, scrubber.GetSample(1U).Sampled == 0U);
    ASTERIA_EXPECT(runner, "the scrub of a run holding the watched page keeps its first words before it zeroes them",
                   scrubber.Zero(PhysicalAddress{watched - PageBytes}, 2U) == FrameScrubError::None,
                   scrubber.GetSample(1U).Physical == watched, scrubber.GetSample(1U).Sampled == 1U,
                   scrubber.GetSample(1U).Words[0U] == 0x0807'0605'0403'0201ULL,
                   scrubber.GetSample(1U).Words[3U] == 0x201F'1E1D'1C1B'1A19ULL, PagesHold(3U, 1U, 0U),
                   scrubber.GetSample(0U).Sampled == 0U);
    g_memory[3U * PageBytes] = 0x77U;
    ASTERIA_EXPECT(runner, "a sampled watch is kept through later scrubs until it is watched again",
                   scrubber.Zero(PhysicalAddress{watched}, 1U) == FrameScrubError::None,
                   scrubber.GetSample(1U).Words[0U] == 0x0807'0605'0403'0201ULL);
    g_memory[3U * PageBytes] = 0x77U;
    scrubber.Watch(1U, watched);
    ASTERIA_EXPECT(
        runner, "watching again forgets the sample and takes the next scrub's", scrubber.GetSample(1U).Sampled == 0U,
        scrubber.Zero(PhysicalAddress{watched}, 1U) == FrameScrubError::None, scrubber.GetSample(1U).Sampled == 1U,
        scrubber.GetSample(1U).Words[0U] == 0x77U, scrubber.GetSample(2U).Physical == PoolStart());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64FrameScrubberTests"};
    Asteria::Test::TestConfiguration(runner);
    Asteria::Test::TestZeroing(runner);
    Asteria::Test::TestSampling(runner);
    return runner.Finish();
}
