// SPDX-License-Identifier: Apache-2.0
#include "Boot/Edid.hpp"

#include <cstring>

#include "Support/EdidFixture.hpp"
#include "Support/TestContext.hpp"

int main()
{
    Asteria::TestSupport::TestContext context{"EdidTests"};
    Asteria::Foundation::UInt8 bytes[128]{};
    Asteria::TestSupport::MakeEdid(bytes);
    Asteria::Boot::EdidTiming output{};
    ASTERIA_EXPECT(context, "preferred timing and identity",
                   Asteria::Boot::ParseEdid(bytes, sizeof(bytes), output) == Asteria::Boot::EdidError::None,
                   output.Width == 1366U, output.Height == 768U, output.Product == 0x1234U,
                   std::strcmp(output.Manufacturer, "AST") == 0, output.PixelClockKhz == 84040U, output.Revision == 4U,
                   output.PreferredTimingFlag);
    const auto before = output;
    for (Asteria::Foundation::Size size{1U}; size < 128U; ++size)
    {
        ASTERIA_EXPECT(context, "all truncated base blocks",
                       Asteria::Boot::ParseEdid(bytes, size, output) == Asteria::Boot::EdidError::Truncated,
                       std::memcmp(&output, &before, sizeof(output)) == 0);
    }
    for (Asteria::Foundation::Size index{0U}; index < 128U; ++index)
    {
        bytes[index] ^= 1U;
        ASTERIA_EXPECT(context, "single-bit corruption",
                       Asteria::Boot::ParseEdid(bytes, sizeof(bytes), output) ==
                           (index < 8U ? Asteria::Boot::EdidError::Header : Asteria::Boot::EdidError::Checksum),
                       std::memcmp(&output, &before, sizeof(output)) == 0);
        bytes[index] ^= 1U;
    }
    constexpr Asteria::Foundation::Size invalidFields[]{18U, 19U, 54U, 56U, 59U, 71U};
    for (const Asteria::Foundation::Size field : invalidFields)
    {
        Asteria::TestSupport::MakeEdid(bytes);
        bytes[field] = field == 71U ? 0x80U : 0U;
        if (field == 19U)
        {
            bytes[field] = 5U;
        }
        if (field == 54U)
        {
            bytes[55U] = 0U;
        }
        if (field == 56U)
        {
            bytes[58U] = 0U;
        }
        if (field == 59U)
        {
            bytes[61U] = 0U;
        }
        Asteria::TestSupport::FixEdidChecksum(bytes);
        ASTERIA_EXPECT(context, "invalid version or first descriptor",
                       Asteria::Boot::ParseEdid(bytes, 128U, output) ==
                           (field < 20U ? Asteria::Boot::EdidError::Version : Asteria::Boot::EdidError::Timing),
                       std::memcmp(&output, &before, sizeof(output)) == 0);
    }
    Asteria::TestSupport::MakeEdid(bytes, 4095U, 4095U);
    ASTERIA_EXPECT(context, "12-bit timing boundary",
                   Asteria::Boot::ParseEdid(bytes, 128U, output) == Asteria::Boot::EdidError::None,
                   output.Width == 4095U, output.Height == 4095U);
    ASTERIA_EXPECT(context, "absence",
                   Asteria::Boot::ParseEdid(nullptr, 128U, output) == Asteria::Boot::EdidError::Absent,
                   Asteria::Boot::ParseEdid(bytes, 0U, output) == Asteria::Boot::EdidError::Absent);
    return context.Finish();
}
