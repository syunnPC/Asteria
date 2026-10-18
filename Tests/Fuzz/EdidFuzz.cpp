// SPDX-License-Identifier: Apache-2.0
#include "Boot/Edid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Support/EdidFixture.hpp"

namespace
{
void Check(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Asteria::Boot::EdidTiming output{};
    output.Width = 0xA5U;
    const auto before = output;
    const auto error = Asteria::Boot::ParseEdid(data, size, output);
    if (error != Asteria::Boot::EdidError::None)
    {
        if (std::memcmp(&before, &output, sizeof(output)) != 0)
        {
            std::abort();
        }
        return;
    }
    if (size < 128U || output.Width == 0U || output.Width > 4095U || output.Height == 0U || output.Height > 4095U ||
        output.PixelClockKhz == 0U || output.Manufacturer[3U] != '\0')
    {
        std::abort();
    }
    Asteria::Boot::EdidTiming baseOnly{};
    if (Asteria::Boot::ParseEdid(data, 128U, baseOnly) != Asteria::Boot::EdidError::None ||
        std::memcmp(&baseOnly, &output, sizeof(output)) != 0)
    {
        std::abort();
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Check(data, size);
    std::uint8_t synthetic[128]{};
    Asteria::TestSupport::MakeEdid(synthetic);
    // Deep parser coverage even when arbitrary input has no valid header.
    for (std::size_t index{8U}; index < 127U && index - 8U < size; ++index)
    {
        synthetic[index] = data[index - 8U];
    }
    Asteria::TestSupport::FixEdidChecksum(synthetic);
    Check(synthetic, sizeof(synthetic));
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t bytes[128]{};
    Asteria::TestSupport::MakeEdid(bytes);
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
    emit(bytes, 127U);
    bytes[127U] ^= 1U;
    emit(bytes, sizeof(bytes));
}
