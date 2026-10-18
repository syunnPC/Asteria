// SPDX-License-Identifier: Apache-2.0
#include "Platform/Firmware/DiagnosticLineSink.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Firmware/DiagnosticUserRecord.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
namespace
{
Asteria::Foundation::Size g_attempts{};
char g_output[128U]{};
Asteria::Foundation::Size g_used{};
void Append(const char* bytes, Asteria::Foundation::Size length) noexcept
{
    for (Asteria::Foundation::Size i{}; i < length && g_used < sizeof(g_output); ++i)
    {
        g_output[g_used++] = bytes[i];
    }
}
} // namespace
namespace Asteria::Platform::Firmware
{
// A competing writer owns the first acquisition. Subsequent acquisitions
// succeed: the old three-call sink would emit an unprefixed body.
void WriteDiagnosticAscii(const char* text) noexcept
{
    if (++g_attempts != 1U)
    {
        Append(text, std::strlen(text));
    }
}
void WriteDiagnosticBytes(const Foundation::UInt8* text, Foundation::Size length) noexcept
{
    if (++g_attempts != 1U)
    {
        Append(reinterpret_cast<const char*>(text), length);
    }
}
void WriteDiagnosticUserLine(const Foundation::UInt8* text, Foundation::Size length) noexcept
{
    if (++g_attempts == 1U)
    {
        return;
    }
    Append("\r\nUSER:", 7U);
    Append(reinterpret_cast<const char*>(text), length);
    Append("\r\n", 2U);
}
} // namespace Asteria::Platform::Firmware
int main()
{
    Asteria::TestSupport::TestContext context{"DiagnosticLineSink"};
    Asteria::Platform::Firmware::DiagnosticLineSink sink{};
    const Asteria::Foundation::UInt8 body[]{'b', 'o', 'd', 'y'};
    sink.WriteLine(body, sizeof(body));
    ASTERIA_EXPECT(context, "a busy prefix drops the whole user line", g_attempts == 1U, g_used == 0U);
    g_attempts = 1U;
    g_used = 0U;
    sink.WriteLine(body, sizeof(body));
    ASTERIA_EXPECT(context, "one acquisition emits prefix body and terminator", g_attempts == 2U, g_used == 13U,
                   std::memcmp(g_output, "\r\nUSER:body\r\n", 13U) == 0);
    for (Asteria::Foundation::Size failAt{}; failAt < 13U; ++failAt)
    {
        Asteria::Foundation::Size attempts{};
        g_used = 0U;
        Asteria::Platform::Firmware::WriteDiagnosticUserRecord(
            [&](const Asteria::Foundation::UInt8 value) noexcept
            {
                if (attempts++ == failAt)
                {
                    return false;
                }
                const char byte{static_cast<char>(value)};
                Append(&byte, 1U);
                return true;
            },
            body, sizeof(body));
        ASTERIA_EXPECT(context, "byte failure never starts another record segment", attempts == failAt + 1U,
                       g_used == failAt, std::memcmp(g_output, "\r\nUSER:body\r\n", failAt) == 0);
    }
    return context.Finish();
}
