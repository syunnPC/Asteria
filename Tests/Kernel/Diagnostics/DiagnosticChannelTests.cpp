// SPDX-License-Identifier: Apache-2.0
#include "Abi/SystemCallAbi.hpp"
#include "Kernel/Diagnostics/DiagnosticChannel.hpp"

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Kernel::Diagnostics::DiagnosticChannel;
using Kernel::Diagnostics::DiagnosticWriteError;

[[nodiscard]] DiagnosticWriteError ValidateBytes(const UInt8 first, const UInt8 second, const UInt8 last) noexcept
{
    const UInt8 text[3]{first, second, last};
    return DiagnosticChannel::Validate(text, sizeof(text));
}

void TestValidation(TestSupport::TestContext& runner) noexcept
{
    UInt8 longest[Abi::DebugWriteMaximumBytes + 1U]{};
    for (UInt8& value : longest)
    {
        value = 'a';
    }
    longest[Abi::DebugWriteMaximumBytes - 1U] = '\n';
    ASTERIA_EXPECT(runner, "printable text ending with LF is accepted up to the maximum size",
                   ValidateBytes(' ', '~', '\n') == DiagnosticWriteError::None,
                   ValidateBytes('\n', '\n', '\n') == DiagnosticWriteError::None,
                   DiagnosticChannel::Validate(longest, Abi::DebugWriteMaximumBytes) == DiagnosticWriteError::None);
    ASTERIA_EXPECT(runner, "empty, oversized, control-byte and unterminated text is rejected",
                   DiagnosticChannel::Validate(longest, 0U) == DiagnosticWriteError::Empty,
                   DiagnosticChannel::Validate(nullptr, 1U) == DiagnosticWriteError::Empty,
                   DiagnosticChannel::Validate(longest, sizeof(longest)) == DiagnosticWriteError::TooLong,
                   ValidateBytes('a', 0x00U, '\n') == DiagnosticWriteError::InvalidByte,
                   ValidateBytes('a', 0x1FU, '\n') == DiagnosticWriteError::InvalidByte,
                   ValidateBytes('a', 0x7FU, '\n') == DiagnosticWriteError::InvalidByte,
                   ValidateBytes('a', '\r', '\n') == DiagnosticWriteError::InvalidByte,
                   ValidateBytes('a', '\t', '\n') == DiagnosticWriteError::InvalidByte,
                   ValidateBytes('a', 0x80U, '\n') == DiagnosticWriteError::InvalidByte,
                   ValidateBytes('a', 'b', 'c') == DiagnosticWriteError::UnterminatedLine);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    Asteria::TestSupport::TestContext runner{"DiagnosticChannelTests"};
    Asteria::Test::TestValidation(runner);
    return runner.Finish();
}
