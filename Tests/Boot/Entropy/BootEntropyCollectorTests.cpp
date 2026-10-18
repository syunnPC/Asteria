// SPDX-License-Identifier: Apache-2.0
// The boot entropy pool (BootEntropyCollector): the documented framing and
// HKDF-SHA256 extraction, the health-test boundaries of each source, and a
// seeded model test in which each source follows a scenario whose verdict is
// known by construction while the samples of all sources interleave at random.

#include <cstdint>
#include <cstdio>

#include "Boot/Entropy/BootEntropyCollector.hpp"
#include "Kernel/Security/Sha256.hpp"

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::BootEntropy;
using Boot::BootEntropySource;
using Boot::GetBootEntropySourceMask;
using Boot::Entropy::BootEntropyCollector;
using Boot::Entropy::BootEntropyError;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Security::HmacSha256;
using Kernel::Security::Sha256DigestBytes;
using TestSupport::Random;

constexpr UInt32 FirmwareRng{GetBootEntropySourceMask(BootEntropySource::FirmwareRng)};
constexpr UInt32 ProcessorRng{GetBootEntropySourceMask(BootEntropySource::ProcessorRng)};
constexpr UInt32 Jitter{GetBootEntropySourceMask(BootEntropySource::CycleCounterJitter)};
constexpr UInt32 FirmwareState{GetBootEntropySourceMask(BootEntropySource::FirmwareState)};
constexpr UInt8 Salt[]{'A', 's', 't', 'e', 'r', 'i', 'a', ' ', 'b', 'o', 'o', 't',
                       ' ', 'e', 'n', 't', 'r', 'o', 'p', 'y', ' ', 'v', '1'};
constexpr Size MinimumDeltas{Boot::Entropy::MinimumCycleCounterDeltas};
constexpr Size Window{Boot::Entropy::AdaptiveProportionWindow};

// The framed input the collector must extract from: per sample its source
// bit, its length as a little-endian 64-bit value, and its bytes.
class FrameLog final
{
public:
    void Add(const UInt32 source, const UInt8* const bytes, const Size sizeBytes) noexcept
    {
        Append(static_cast<UInt8>(source));
        for (Size index{0U}; index < 8U; ++index)
        {
            Append(static_cast<UInt8>(static_cast<UInt64>(sizeBytes) >> (index * 8U)));
        }
        for (Size index{0U}; index < sizeBytes; ++index)
        {
            Append(bytes[index]);
        }
    }

    // Whether seed is HKDF-Extract(salt, log): HMAC-SHA256 in one pass.
    [[nodiscard]] bool IsExtractedAs(const UInt8 (&seed)[Boot::BootEntropySeedBytes]) const noexcept
    {
        HmacSha256 extract{Salt, sizeof(Salt)};
        extract.Update(m_bytes, m_count);
        UInt8 expected[Sha256DigestBytes]{};
        extract.Finish(expected);
        for (Size index{0U}; index < Sha256DigestBytes; ++index)
        {
            if (seed[index] != expected[index])
            {
                return false;
            }
        }
        return !m_overflowed;
    }

private:
    void Append(const UInt8 value) noexcept
    {
        if (m_count == sizeof(m_bytes))
        {
            m_overflowed = true;
            return;
        }
        m_bytes[m_count] = value;
        ++m_count;
    }

    UInt8 m_bytes[65'536U]{};
    Size m_count{0U};
    bool m_overflowed{false};
};

// Feeds a collector and its log together.
class Feed final
{
public:
    explicit Feed(FrameLog& log) noexcept : m_log{log}
    {
    }

    void Generator(const BootEntropySource source, const UInt8* const bytes, const Size sizeBytes) noexcept
    {
        m_collector.AddGeneratorBlock(source, bytes, sizeBytes);
        m_log.Add(GetBootEntropySourceMask(source), bytes, sizeBytes);
    }

    void Counter(const UInt64 counter) noexcept
    {
        m_collector.AddCycleCounterSample(counter);
        UInt8 bytes[8U]{};
        for (Size index{0U}; index < 8U; ++index)
        {
            bytes[index] = static_cast<UInt8>(counter >> (index * 8U));
        }
        m_log.Add(Jitter, bytes, sizeof(bytes));
    }

    // One counter reading, then one reading per delta.
    void Deltas(const UInt64* const deltas, const Size count) noexcept
    {
        Counter(m_counter);
        for (Size index{0U}; index < count; ++index)
        {
            Delta(deltas[index]);
        }
    }

    // One further reading, delta after the previous one.
    void Delta(const UInt64 delta) noexcept
    {
        m_counter += delta;
        Counter(m_counter);
    }

    void State(const UInt8* const bytes, const Size sizeBytes) noexcept
    {
        m_collector.AddFirmwareState(bytes, sizeBytes);
        m_log.Add(FirmwareState, bytes, sizeBytes);
    }

    [[nodiscard]] Foundation::Result<BootEntropy, BootEntropyError> Finish() noexcept
    {
        return m_collector.Finish();
    }

    BootEntropyCollector& Collector() noexcept
    {
        return m_collector;
    }

private:
    BootEntropyCollector m_collector{};
    FrameLog& m_log;
    UInt64 m_counter{0x7000'0000'0000'0000ULL};
};

constinit FrameLog g_log{};
constinit UInt64 g_deltas[4U * 1024U]{};

// Deltas that pass every jitter test: they cycle through 91 values.
void VariedDeltas(const Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        g_deltas[index] = 100U + ((index * 37U) % 91U);
    }
}

[[nodiscard]] UInt32 SourcesOf(Feed& feed, BootEntropyError& error) noexcept
{
    const auto result = feed.Finish();
    error = result.GetError();
    return result.HasValue() ? result.GetValue().Sources : 0U;
}

[[nodiscard]] bool OnlyJitterIsCredited(const Size deltaCount) noexcept
{
    g_log = FrameLog{};
    Feed feed{g_log};
    feed.Deltas(g_deltas, deltaCount);
    BootEntropyError error{};
    return SourcesOf(feed, error) == Jitter;
}

void TestKnownAnswer(TestContext& runner) noexcept
{
    UInt8 block[32U]{};
    for (Size index{0U}; index < sizeof(block); ++index)
    {
        block[index] = static_cast<UInt8>(index);
    }
    constexpr UInt8 Time[]{'t', 'i', 'm', 'e'};
    BootEntropyCollector collector{};
    collector.AddGeneratorBlock(BootEntropySource::FirmwareRng, block, sizeof(block));
    collector.AddCycleCounterSample(0x1122'3344'5566'7788ULL);
    collector.AddFirmwareState(Time, sizeof(Time));
    const auto result = collector.Finish();
    // HMAC-SHA256("Asteria boot entropy v1", frames) from an independent implementation.
    constexpr UInt8 Expected[Sha256DigestBytes]{
        0x5EU, 0x90U, 0x51U, 0x70U, 0x70U, 0x82U, 0xA0U, 0xBBU, 0x37U, 0xECU, 0x83U, 0xEBU, 0x9BU, 0x11U, 0xB3U, 0x27U,
        0x45U, 0x5CU, 0x6CU, 0x1DU, 0x00U, 0xC5U, 0x84U, 0x96U, 0xC4U, 0x47U, 0x28U, 0xD4U, 0x20U, 0x3BU, 0x28U, 0x1EU,
    };
    bool seedMatches{result.HasValue()};
    for (Size index{0U}; seedMatches && (index < Sha256DigestBytes); ++index)
    {
        seedMatches = result.GetValue().Seed[index] == Expected[index];
    }
    ASTERIA_EXPECT(runner, "the seed is HKDF-Extract of the framed samples; the sources are credited as documented",
                   result.HasValue(), seedMatches, result.GetValue().Sources == (FirmwareRng | FirmwareState),
                   result.GetValue().Reserved == 0U);
    ASTERIA_EXPECT(runner, "a collection ends with its first Finish", !collector.Finish().HasValue(),
                   collector.Finish().GetError() == BootEntropyError::AlreadyFinished);
}

void TestGeneratorHealth(TestContext& runner) noexcept
{
    UInt8 bytes[40U]{};
    for (Size index{0U}; index < sizeof(bytes); ++index)
    {
        bytes[index] = static_cast<UInt8>((index * 29U) + 1U);
    }
    BootEntropyError error{};
    {
        Feed feed{g_log};
        feed.Generator(BootEntropySource::ProcessorRng, bytes, 8U);
        feed.Generator(BootEntropySource::ProcessorRng, bytes + 8U, 8U);
        feed.Generator(BootEntropySource::ProcessorRng, bytes + 16U, 8U);
        feed.Generator(BootEntropySource::ProcessorRng, bytes + 24U, 8U);
        ASTERIA_EXPECT(runner, "four distinct 8-byte blocks credit the processor generator",
                       SourcesOf(feed, error) == ProcessorRng);
    }
    {
        Feed feed{g_log};
        feed.Generator(BootEntropySource::FirmwareRng, bytes, 16U);
        feed.Generator(BootEntropySource::FirmwareRng, bytes + 16U, 15U);
        ASTERIA_EXPECT(runner, "31 healthy bytes are not enough to credit a generator", SourcesOf(feed, error) == 0U,
                       error == BootEntropyError::NoHealthySource);
    }
    {
        Feed feed{g_log};
        feed.Generator(BootEntropySource::FirmwareRng, bytes, 7U);
        feed.Generator(BootEntropySource::FirmwareRng, bytes + 8U, 32U);
        ASTERIA_EXPECT(runner, "a block shorter than eight bytes fails the generator for the whole boot",
                       SourcesOf(feed, error) == 0U);
    }
    {
        UInt8 uniform[32U]{};
        Feed feed{g_log};
        feed.Generator(BootEntropySource::FirmwareRng, bytes, 32U);
        feed.Generator(BootEntropySource::FirmwareRng, uniform, sizeof(uniform));
        feed.Generator(BootEntropySource::ProcessorRng, bytes, 32U);
        ASTERIA_EXPECT(runner, "a block of one repeated byte fails its generator only",
                       SourcesOf(feed, error) == ProcessorRng);
    }
    {
        UInt8 repeated[16U]{};
        for (Size index{0U}; index < sizeof(repeated); ++index)
        {
            repeated[index] = bytes[index < 8U ? index : index + 8U];
        }
        Feed feed{g_log};
        feed.Generator(BootEntropySource::FirmwareRng, bytes, 16U);
        feed.Generator(BootEntropySource::FirmwareRng, repeated, sizeof(repeated));
        feed.Generator(BootEntropySource::FirmwareRng, bytes + 24U, 16U);
        ASTERIA_EXPECT(runner, "a block beginning with the previous block's first eight bytes fails the generator",
                       SourcesOf(feed, error) == 0U);
    }
    {
        // Each block differs from the previous one in a single bit of its
        // first eight bytes (in the first byte, the last one, and next to a
        // byte with its top bit set), and is therefore new; blocks equal but
        // for byte 0 or 1 are not uniform.
        UInt8 blocks[5U][8U]{};
        for (Size index{0U}; index < 8U; ++index)
        {
            blocks[0U][index] = static_cast<UInt8>(0x80U | (index * 3U));
        }
        constexpr Size FlippedByte[4U]{0U, 7U, 4U, 0U};
        constexpr UInt8 FlippedBit[4U]{0x01U, 0x02U, 0x01U, 0x10U};
        for (Size block{1U}; block < 5U; ++block)
        {
            for (Size index{0U}; index < 8U; ++index)
            {
                blocks[block][index] = blocks[block - 1U][index];
            }
            blocks[block][FlippedByte[block - 1U]] ^= FlippedBit[block - 1U];
        }
        UInt8 onlyByteOne[8U]{7U, 9U, 7U, 7U, 7U, 7U, 7U, 7U};
        UInt8 onlyByteZero[8U]{9U, 7U, 7U, 7U, 7U, 7U, 7U, 7U};
        Feed feed{g_log};
        for (Size index{0U}; index < 4U; ++index)
        {
            feed.Generator(BootEntropySource::FirmwareRng, blocks[index], sizeof(blocks[index]));
        }
        feed.Generator(BootEntropySource::ProcessorRng, onlyByteOne, sizeof(onlyByteOne));
        feed.Generator(BootEntropySource::ProcessorRng, onlyByteZero, sizeof(onlyByteZero));
        feed.Generator(BootEntropySource::ProcessorRng, blocks[4U], sizeof(blocks[4U]));
        feed.Generator(BootEntropySource::ProcessorRng, blocks[1U], sizeof(blocks[1U]));
        ASTERIA_EXPECT(runner, "one differing bit or byte makes a block new and not uniform",
                       SourcesOf(feed, error) == (FirmwareRng | ProcessorRng));
    }
    {
        Feed feed{g_log};
        feed.Generator(BootEntropySource::CycleCounterJitter, bytes, 32U);
        feed.Generator(BootEntropySource::FirmwareState, bytes + 8U, 32U);
        feed.Generator(static_cast<BootEntropySource>(1U << 7U), bytes, 32U);
        ASTERIA_EXPECT(runner, "a block from a source that is no generator credits nothing",
                       SourcesOf(feed, error) == 0U);
    }
}

void TestJitterHealth(TestContext& runner) noexcept
{
    VariedDeltas(sizeof(g_deltas) / sizeof(g_deltas[0U]));
    ASTERIA_EXPECT(runner, "the jitter source is credited from its minimum number of deltas on",
                   !OnlyJitterIsCredited(MinimumDeltas - 1U), OnlyJitterIsCredited(MinimumDeltas),
                   OnlyJitterIsCredited(3U * MinimumDeltas));

    g_deltas[600U] = 0U;
    ASTERIA_EXPECT(runner, "a counter that did not advance fails the jitter source",
                   !OnlyJitterIsCredited(MinimumDeltas + 10U));

    // Repetition count test: 31 equal consecutive deltas pass, 32 fail.
    VariedDeltas(MinimumDeltas + 64U);
    for (Size index{300U}; index < 331U; ++index)
    {
        g_deltas[index] = 5U;
    }
    const bool thirtyOnePass = OnlyJitterIsCredited(MinimumDeltas + 64U);
    g_deltas[331U] = 5U;
    ASTERIA_EXPECT(runner, "the repetition count test fails exactly at its cutoff", thirtyOnePass,
                   !OnlyJitterIsCredited(MinimumDeltas + 64U));

    // Adaptive proportion test: in the second window, deltas equal to the
    // window's first delta at every position but each fourth one.
    VariedDeltas(3U * Window);
    // Every other delta of the window differs from the reference and from
    // each other, so the repetition count test never fires.
    const auto fillWindow = [](const Size window, const Size matches) noexcept
    {
        const Size first = window * Window;
        const UInt64 reference = g_deltas[first];
        Size placed{1U};
        for (Size offset{1U}; offset < Window; ++offset)
        {
            const bool match = ((offset % 4U) != 0U) && (placed < matches);
            g_deltas[first + offset] = match ? reference : reference + 1U + offset;
            placed += match ? 1U : 0U;
        }
    };
    fillWindow(1U, Boot::Entropy::AdaptiveProportionCutoff - 1U);
    fillWindow(2U, Boot::Entropy::AdaptiveProportionCutoff - 1U);
    const bool belowCutoffPasses = OnlyJitterIsCredited(3U * Window);
    VariedDeltas(3U * Window);
    fillWindow(1U, Boot::Entropy::AdaptiveProportionCutoff);
    ASTERIA_EXPECT(runner, "the adaptive proportion test counts per window and fails at its cutoff", belowCutoffPasses,
                   !OnlyJitterIsCredited(3U * Window));
}

// Scenarios with verdicts known by construction.
enum class GeneratorScenario : UInt8
{
    Absent,
    Healthy,
    TooFew,
    ShortBlock,
    UniformBlock,
    RepeatedBlock,
};

enum class JitterScenario : UInt8
{
    Absent,
    Healthy,
    TooFew,
    Stuck,
    Repetition,
};

struct Operation
{
    UInt8 Kind; // 0 FirmwareRng, 1 ProcessorRng, 2 counter delta, 3 firmware state
    UInt8 Size;
    UInt64 Value; // counter delta, or the seed of the block bytes
};

constinit Operation g_operations[4096U]{};

[[nodiscard]] Size PlanGenerator(Random& random, const UInt8 kind, const GeneratorScenario scenario,
                                 Operation* const operations) noexcept
{
    if (scenario == GeneratorScenario::Absent)
    {
        return 0U;
    }
    if (scenario == GeneratorScenario::TooFew)
    {
        // One to three healthy 8-byte blocks: at most 24 bytes.
        const Size count = 1U + random.Below(3U);
        for (Size index{0U}; index < count; ++index)
        {
            operations[index] = Operation{kind, 8U, random.Next() | 1U};
        }
        return count;
    }
    // Healthy blocks of 8 to 16 bytes, 32 to 95 bytes in all (two blocks at least).
    const Size target = 32U + random.Below(64U);
    Size count{0U};
    Size bytes{0U};
    while (bytes < target)
    {
        operations[count] = Operation{kind, static_cast<UInt8>(8U + random.Below(9U)), random.Next() | 1U};
        bytes += operations[count].Size;
        ++count;
    }
    const Size position = random.Below(count - 1U);
    switch (scenario)
    {
    case GeneratorScenario::ShortBlock:
        operations[position].Size = static_cast<UInt8>(1U + random.Below(7U));
        break;
    case GeneratorScenario::UniformBlock:
        operations[position].Value = 0U;
        break;
    case GeneratorScenario::RepeatedBlock:
        operations[position + 1U].Value = operations[position].Value;
        break;
    case GeneratorScenario::Absent:
    case GeneratorScenario::Healthy:
    case GeneratorScenario::TooFew:
        break;
    }
    return count;
}

// Block bytes: a uniform block for Value 0, otherwise a pseudo-random fill
// whose first eight bytes are Value itself.
void FillBlock(const Operation& operation, UInt8 (&bytes)[16U]) noexcept
{
    Random fill{operation.Value};
    for (Size index{0U}; index < sizeof(bytes); ++index)
    {
        bytes[index] = operation.Value == 0U
                           ? UInt8{0xA5U}
                           : static_cast<UInt8>(index < 8U ? (operation.Value >> (index * 8U)) : fill.Next());
    }
}

[[nodiscard]] Size PlanJitter(Random& random, const JitterScenario scenario, Operation* const operations) noexcept
{
    if (scenario == JitterScenario::Absent)
    {
        return 0U;
    }
    const Size count =
        scenario == JitterScenario::TooFew ? 1U + random.Below(MinimumDeltas - 1U) : MinimumDeltas + random.Below(256U);
    for (Size index{0U}; index < count; ++index)
    {
        operations[index] = Operation{2U, 8U, 1U + random.Below(UInt64{1U} << 20U)};
    }
    if (scenario == JitterScenario::Stuck)
    {
        operations[random.Below(count)].Value = 0U;
    }
    if (scenario == JitterScenario::Repetition)
    {
        const Size position = random.Below(count - Boot::Entropy::RepetitionCountCutoff);
        for (Size index{0U}; index < Boot::Entropy::RepetitionCountCutoff; ++index)
        {
            operations[position + index].Value = 9U;
        }
    }
    return count;
}

void TestModel(TestContext& runner) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(1U, 96U);
    for (UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        Random random{seed};
        const auto firmwareScenario = static_cast<GeneratorScenario>(random.Below(6U));
        const auto processorScenario = static_cast<GeneratorScenario>(random.Below(6U));
        const auto jitterScenario = static_cast<JitterScenario>(random.Below(5U));
        const Size stateCount = random.Below(3U);

        // Plan each source, then interleave the plans at random while keeping
        // each source's own order.
        Operation* const firmware = g_operations;
        const Size firmwareCount = PlanGenerator(random, 0U, firmwareScenario, firmware);
        Operation* const processor = firmware + firmwareCount;
        const Size processorCount = PlanGenerator(random, 1U, processorScenario, processor);
        Operation* const jitter = processor + processorCount;
        const Size jitterCount = PlanJitter(random, jitterScenario, jitter);
        const Size cursorLimits[4U]{firmwareCount, processorCount, jitterCount, stateCount};
        Operation* const plans[4U]{firmware, processor, jitter, nullptr};
        Size cursors[4U]{};

        g_log = FrameLog{};
        Feed feed{g_log};
        bool counterStarted{false};
        while ((cursors[0U] < cursorLimits[0U]) || (cursors[1U] < cursorLimits[1U]) ||
               (cursors[2U] < cursorLimits[2U]) || (cursors[3U] < cursorLimits[3U]))
        {
            const Size kind = random.Below(4U);
            if (cursors[kind] == cursorLimits[kind])
            {
                continue;
            }
            if (kind == 3U)
            {
                const UInt64 value = random.Next();
                UInt8 state[8U]{};
                for (Size index{0U}; index < sizeof(state); ++index)
                {
                    state[index] = static_cast<UInt8>(value >> (index * 8U));
                }
                feed.State(state, 1U + random.Below(8U));
            }
            else if (kind == 2U)
            {
                if (!counterStarted)
                {
                    feed.Deltas(nullptr, 0U);
                    counterStarted = true;
                }
                feed.Delta(plans[kind][cursors[kind]].Value);
            }
            else
            {
                UInt8 bytes[16U]{};
                FillBlock(plans[kind][cursors[kind]], bytes);
                feed.Generator(kind == 0U ? BootEntropySource::FirmwareRng : BootEntropySource::ProcessorRng, bytes,
                               plans[kind][cursors[kind]].Size);
            }
            ++cursors[kind];
        }

        const UInt32 expected = (firmwareScenario == GeneratorScenario::Healthy ? FirmwareRng : 0U) |
                                (processorScenario == GeneratorScenario::Healthy ? ProcessorRng : 0U) |
                                (jitterScenario == JitterScenario::Healthy ? Jitter : 0U);
        const auto result = feed.Finish();
        const bool matches =
            (expected == 0U) ? (!result.HasValue() && (result.GetError() == BootEntropyError::NoHealthySource))
                             : (result.HasValue() &&
                                (result.GetValue().Sources == (expected | (stateCount != 0U ? FirmwareState : 0U))) &&
                                g_log.IsExtractedAs(result.GetValue().Seed));
        if (!ASTERIA_EXPECT(runner, "the collector credits the sources the scenarios allow and extracts the log",
                            matches))
        {
            static_cast<void>(std::fprintf(
                stderr, "  seed %llu: scenarios firmware=%u processor=%u jitter=%u state=%zu, expected sources 0x%x\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned int>(firmwareScenario),
                static_cast<unsigned int>(processorScenario), static_cast<unsigned int>(jitterScenario), stateCount,
                static_cast<unsigned int>(expected)));
            return;
        }
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"BootEntropyCollectorTests"};
    Asteria::Test::TestKnownAnswer(runner);
    Asteria::Test::TestGeneratorHealth(runner);
    Asteria::Test::TestJitterHealth(runner);
    Asteria::Test::TestModel(runner);
    return runner.Finish();
}
