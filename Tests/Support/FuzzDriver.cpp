// SPDX-License-Identifier: Apache-2.0
// Deterministic driver for libFuzzer-style harnesses in GCC builds.
//
// A harness defines LLVMFuzzerTestOneInput and, optionally,
// AsteriaFuzzSeeds, which emits valid seed inputs. The driver replays the
// seeds and any corpus files named on the command line, then applies a fixed
// number of seeded random mutations to the seeds and the first files. Files
// beyond the seed capacity are replayed once, so `--iterations 0 FILE...`
// replays a whole campaign corpus (for example in the coverage profile). The
// same harness links unchanged against libFuzzer (-fsanitize=fuzzer) for
// open-ended campaigns.
//
// Usage: <harness> [--iterations N] [--seed S] [--write-seeds DIR] [FILE...]

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Support/Random.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
extern "C" [[gnu::weak]] void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t* data, std::size_t size));

namespace
{
constexpr std::size_t MaximumInputBytes{4096U};
constexpr std::size_t MaximumSeeds{64U};

struct Seed
{
    std::uint8_t Bytes[MaximumInputBytes];
    std::size_t Size;
};

Seed g_seeds[MaximumSeeds];
std::size_t g_seedCount{0U};
const char* g_writeDirectory{nullptr};

void EmitSeed(const std::uint8_t* const data, const std::size_t size)
{
    if ((g_seedCount >= MaximumSeeds) || (size > MaximumInputBytes))
    {
        static_cast<void>(std::fprintf(stderr, "FAIL: seed capacity exceeded\n"));
        std::abort();
    }
    Seed& seed = g_seeds[g_seedCount++];
    std::memcpy(seed.Bytes, data, size);
    seed.Size = size;
    if (g_writeDirectory != nullptr)
    {
        char path[512];
        static_cast<void>(std::snprintf(path, sizeof(path), "%s/seed-%02zu", g_writeDirectory, g_seedCount));
        std::FILE* const file = std::fopen(path, "wb");
        if ((file == nullptr) || (std::fwrite(data, 1U, size, file) != size) || (std::fclose(file) != 0))
        {
            static_cast<void>(std::fprintf(stderr, "FAIL: cannot write %s\n", path));
            std::abort();
        }
    }
}

[[nodiscard]] bool LoadFile(const char* const path, Seed& seed)
{
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    seed.Size = std::fread(seed.Bytes, 1U, MaximumInputBytes, file);
    static_cast<void>(std::fclose(file));
    return true;
}

// One structural mutation of the kinds libFuzzer applies most often.
void Mutate(Asteria::TestSupport::Random& random, Seed& input)
{
    const std::uint64_t kind = random.Below(7U);
    if ((input.Size == 0U) || (kind == 0U))
    {
        if (input.Size < MaximumInputBytes)
        {
            const std::size_t at = static_cast<std::size_t>(random.Below(input.Size + 1U));
            std::memmove(input.Bytes + at + 1U, input.Bytes + at, input.Size - at);
            input.Bytes[at] = static_cast<std::uint8_t>(random.Next());
            ++input.Size;
        }
        return;
    }
    const std::size_t at = static_cast<std::size_t>(random.Below(input.Size));
    switch (kind)
    {
    case 1U:
        input.Bytes[at] = static_cast<std::uint8_t>(input.Bytes[at] ^ (1U << random.Below(8U)));
        break;
    case 2U:
        input.Bytes[at] = static_cast<std::uint8_t>(random.Next());
        break;
    case 3U:
    {
        constexpr std::uint8_t interesting[]{0x00U, 0x01U, 0x7FU, 0x80U, 0xFEU, 0xFFU};
        input.Bytes[at] = interesting[random.Below(sizeof(interesting))];
        break;
    }
    case 4U:
        std::memmove(input.Bytes + at, input.Bytes + at + 1U, input.Size - at - 1U);
        --input.Size;
        break;
    case 5U:
        input.Size = at;
        break;
    default:
    {
        // Overwrite with bytes from another seed at the same offset.
        const Seed& other = g_seeds[random.Below(g_seedCount)];
        const std::size_t length = static_cast<std::size_t>(random.Below(16U)) + 1U;
        for (std::size_t index{0U}; (index < length) && (at + index < input.Size) && (at + index < other.Size); ++index)
        {
            input.Bytes[at + index] = other.Bytes[at + index];
        }
        break;
    }
    }
}
// Returns true for the options that take a value; their values follow them.
[[nodiscard]] bool IsOption(const char* const argument)
{
    return (std::strcmp(argument, "--iterations") == 0) || (std::strcmp(argument, "--seed") == 0) ||
           (std::strcmp(argument, "--write-seeds") == 0);
}
} // namespace

int main(int argumentCount, char** arguments)
{
    std::uint64_t iterations{20000U};
    std::uint64_t seedValue{1U};
    for (int index{1}; index < argumentCount; ++index)
    {
        if (!IsOption(arguments[index]))
        {
            continue;
        }
        if (index + 1 >= argumentCount)
        {
            static_cast<void>(std::fprintf(stderr, "FAIL: %s needs a value\n", arguments[index]));
            return 2;
        }
        const char* const value = arguments[index + 1];
        if (std::strcmp(arguments[index], "--iterations") == 0)
        {
            iterations = std::strtoull(value, nullptr, 0);
        }
        else if (std::strcmp(arguments[index], "--seed") == 0)
        {
            seedValue = std::strtoull(value, nullptr, 0);
        }
        else
        {
            g_writeDirectory = value;
        }
        ++index;
    }
    if (AsteriaFuzzSeeds != nullptr)
    {
        AsteriaFuzzSeeds(&EmitSeed);
    }

    // Corpus files join the seeds while capacity remains; later files are
    // replayed once, so any number of files can be replayed.
    Seed scratch{};
    std::size_t replayedFiles{0U};
    for (int index{1}; index < argumentCount; ++index)
    {
        if (IsOption(arguments[index]))
        {
            ++index;
            continue;
        }
        if (!LoadFile(arguments[index], scratch))
        {
            static_cast<void>(std::fprintf(stderr, "FAIL: cannot read %s\n", arguments[index]));
            return 2;
        }
        if (g_seedCount < MaximumSeeds)
        {
            EmitSeed(scratch.Bytes, scratch.Size);
        }
        else
        {
            static_cast<void>(LLVMFuzzerTestOneInput(scratch.Bytes, scratch.Size));
            ++replayedFiles;
        }
    }
    if (g_seedCount == 0U)
    {
        const std::uint8_t empty{0U};
        EmitSeed(&empty, 0U);
    }
    for (std::size_t index{0U}; index < g_seedCount; ++index)
    {
        static_cast<void>(LLVMFuzzerTestOneInput(g_seeds[index].Bytes, g_seeds[index].Size));
    }
    Asteria::TestSupport::Random random{seedValue};
    for (std::uint64_t iteration{0U}; iteration < iterations; ++iteration)
    {
        scratch = g_seeds[random.Below(g_seedCount)];
        const std::uint64_t rounds = random.Below(8U) + 1U;
        for (std::uint64_t round{0U}; round < rounds; ++round)
        {
            Mutate(random, scratch);
        }
        static_cast<void>(LLVMFuzzerTestOneInput(scratch.Bytes, scratch.Size));
    }
    static_cast<void>(std::printf("PASS: replayed %zu seeds, %zu further files, and %llu mutations (seed %llu)\n",
                                  g_seedCount, replayedFiles, static_cast<unsigned long long>(iterations),
                                  static_cast<unsigned long long>(seedValue)));
    return 0;
}
