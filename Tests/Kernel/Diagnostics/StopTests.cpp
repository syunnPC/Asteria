// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Diagnostics/StopRecord.hpp"
#include "Kernel/Diagnostics/StopDisplay.hpp"
#include "Kernel/Diagnostics/StopScreen.hpp"

#include <barrier>
#include <thread>
#include <cstring>

#include "Support/Random.hpp"

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Kernel::Diagnostics::StopCode;
using Kernel::Diagnostics::StopRecord;
using Kernel::Diagnostics::StopState;

void Vocabulary(TestSupport::TestContext& context) noexcept
{
    UInt32 seen{0U};
    for (const auto& entry : Kernel::Diagnostics::StopCodeNames)
    {
        const UInt32 code = static_cast<UInt32>(entry.Code);
        ASTERIA_EXPECT(context, "codes are unique and names use the common table", code > 0U, code < 32U,
                       (seen & (1U << code)) == 0U, Kernel::Diagnostics::StopName(entry.Code) == entry.Name);
        seen |= 1U << code;
        char upper[64U]{};
        Size length{0U};
        for (const char* cursor = entry.Name; *cursor != '\0'; ++cursor)
        {
            upper[length++] = *cursor >= 'a' && *cursor <= 'z' ? static_cast<char>(*cursor - 'a' + 'A') : *cursor;
            ASTERIA_EXPECT(context, "names are ASCII letters",
                           (*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z'));
        }
        ASTERIA_EXPECT(context, "stop names contain no legacy failure tokens", std::strstr(upper, "FAIL") == nullptr,
                       std::strstr(upper, "ERROR") == nullptr, std::strstr(upper, "FAULT") == nullptr);
    }
    ASTERIA_EXPECT(context, "unknown values have a diagnostic name",
                   Kernel::Diagnostics::StopName(static_cast<StopCode>(0U))[0U] == 'U');
}

void StateModel(TestSupport::TestContext& context) noexcept
{
    const auto seeds = TestSupport::GetSeedRange(0x250018U, 256U);
    for (UInt64 trial{seeds.First}; trial < seeds.First + seeds.Count; ++trial)
    {
        TestSupport::Random random{trial};
        StopRecord record{};
        StopState model{StopState::Running};
        UInt32 modelCode{0U};
        UInt64 modelParameters[4U]{};
        for (Size step{0U}; step < 32U; ++step)
        {
            const Size failuresBefore = context.GetFailureCount();
            const UInt64 seed = random.Next();
            if ((seed & 1U) == 0U)
            {
                const bool accepted =
                    Kernel::Diagnostics::BeginStop(record, StopCode::TestInjected, seed, step, trial, 0U);
                ASTERIA_EXPECT(context, "model admits only a running record",
                               accepted == (model == StopState::Running));
                if (model == StopState::Running)
                {
                    model = StopState::Stopping;
                    modelCode = static_cast<UInt32>(StopCode::TestInjected);
                    modelParameters[0U] = seed;
                    modelParameters[1U] = step;
                    modelParameters[2U] = trial;
                }
            }
            else if ((seed & 2U) == 0U)
            {
                Kernel::Diagnostics::MarkStopUnconfirmed(record);
                if (model == StopState::Running || model == StopState::Stopping)
                {
                    model = StopState::Unconfirmed;
                }
            }
            else
            {
                Kernel::Diagnostics::FreezeStop(record);
                model = StopState::Frozen;
            }
            ASTERIA_EXPECT(context, "record agrees with state model without overwriting payload",
                           record.State == static_cast<UInt64>(model), record.Code == modelCode);
            for (Size i{0U}; i < 4U; ++i)
            {
                ASTERIA_EXPECT(context, "freezing retains parameters", record.Parameters[i] == modelParameters[i]);
            }
            if (context.GetFailureCount() != failuresBefore)
            {
                static_cast<void>(std::fprintf(stderr, "stop model seed %llu step %zu\n",
                                               static_cast<unsigned long long>(trial), step));
                return;
            }
        }
    }
}

void ConcurrentLatch(TestSupport::TestContext& context)
{
    Kernel::Diagnostics::StopLatch latch;
    StopRecord records[2U]{};
    bool winners[2U]{};
    std::barrier ready{2};
    const auto stop = [&](const Size cpu)
    {
        static_cast<void>(Kernel::Diagnostics::BeginStop(records[cpu], StopCode::TestInjected, cpu, 0U, 0U, 0U));
        ready.arrive_and_wait();
        winners[cpu] = latch.TryLatch(cpu);
        Kernel::Diagnostics::FreezeStop(records[cpu]);
    };
    std::thread first{stop, 0U};
    std::thread second{stop, 1U};
    first.join();
    second.join();
    ASTERIA_EXPECT(context, "two contenders produce one latch winner", winners[0U] != winners[1U],
                   latch.Owner() == (winners[0U] ? 1U : 2U));
    for (Size cpu{0U}; cpu < 2U; ++cpu)
    {
        ASTERIA_EXPECT(context, "each CPU retains its own stop", records[cpu].Parameters[0U] == cpu,
                       records[cpu].State == static_cast<UInt64>(StopState::Frozen));
    }
}

void DisplayExtent(TestSupport::TestContext& context) noexcept
{
    // A GOP framebuffer only page aligned, with pitched size above 4 MiB.
    Boot::BootDisplay display{0xE0001000U, 9'000'000U, 1920U, 1080U, 2048U, Boot::BootDisplayFormatBgrx};
    constexpr UInt64 capacity{UInt64{1U} << 30U};
    ASTERIA_EXPECT(context, "full pitch extent without 2 MiB base alignment",
                   Kernel::Diagnostics::StopDisplayBytes(display, capacity) == 2048U * 1080U * 4U);
    display.FrameBufferBytes = 4'194'304U;
    ASTERIA_EXPECT(context, "reject truncated framebuffer",
                   Kernel::Diagnostics::StopDisplayBytes(display, capacity) == 0U);
    display.FrameBufferBytes = 9'000'000U;
    ASTERIA_EXPECT(context, "reject insufficient reserved aperture",
                   Kernel::Diagnostics::StopDisplayBytes(display, 4096U) == 0U);
    display.FrameBufferBase = 0xE0001001U;
    ASTERIA_EXPECT(context, "reject unaligned base", Kernel::Diagnostics::StopDisplayBytes(display, capacity) == 0U);
    display.FrameBufferBase = ~UInt64{4095U};
    ASTERIA_EXPECT(context, "reject physical end overflow",
                   Kernel::Diagnostics::StopDisplayBytes(display, capacity) == 0U);
    display.FrameBufferBase = 0xE0001000U;
    display.Height = display.PixelsPerScanLine = 0xFFFF'FFFFU;
    ASTERIA_EXPECT(context, "reject byte extent multiplication overflow",
                   Kernel::Diagnostics::StopDisplayBytes(display, ~UInt64{0U}) == 0U);
}

void GlyphPixels(TestSupport::TestContext& context) noexcept
{
    UInt32 pixels[24U * 16U]{};
    constexpr char text[]{"A0"};
    for (UInt32 format{0U}; format < 2U; ++format)
    {
        const bool rgbx = format != 0U;
        Kernel::Diagnostics::StopScreen screen{pixels, 24U, 16U, 24U, rgbx};
        screen.Clear();
        screen.Text(text);
        for (UInt32 y{0U}; y < 16U; ++y)
        {
            // Independently pinned public-domain glyph rows for 'A' and '0'.
            constexpr Foundation::UInt8 a[16U]{0x00U, 0x00U, 0x38U, 0x38U, 0x38U, 0x6CU, 0x6CU, 0x6CU,
                                               0x7CU, 0xC6U, 0xC6U, 0xC6U, 0x00U, 0x00U, 0x00U, 0x00U};
            constexpr Foundation::UInt8 zero[16U]{0x00U, 0x00U, 0x7CU, 0xC6U, 0xC6U, 0xC6U, 0xD6U, 0xD6U,
                                                  0xC6U, 0xC6U, 0xC6U, 0x7CU, 0x00U, 0x00U, 0x00U, 0x00U};
            for (UInt32 x{0U}; x < 16U; ++x)
            {
                const UInt32 expected = ((x < 8U ? a[y] : zero[y]) & (0x80U >> (x % 8U))) != 0U
                                            ? 0x00FFFFFFU
                                            : (rgbx ? 0x00A68C00U : 0x00008CA6U);
                ASTERIA_EXPECT(context, "known string pixels and format packing", pixels[y * 24U + x] == expected);
            }
            ASTERIA_EXPECT(context, "margin cleared with 32-bit stores",
                           pixels[y * 24U + 23U] == (rgbx ? 0x00A68C00U : 0x00008CA6U));
        }
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext context{"StopTests"};
    Asteria::Test::Vocabulary(context);
    Asteria::Test::StateModel(context);
    Asteria::Test::ConcurrentLatch(context);
    Asteria::Test::GlyphPixels(context);
    Asteria::Test::DisplayExtent(context);
    return context.Finish();
}
