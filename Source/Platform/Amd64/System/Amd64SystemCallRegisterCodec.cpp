// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/System/Amd64SystemCallRegisterCodec.hpp"

namespace Asteria::Platform::Amd64::System
{
Kernel::System::SystemCallRequest
Amd64SystemCallRegisterCodec::DecodeRequest(const Amd64SystemCallRegisterImage& registers) noexcept
{
    return Kernel::System::SystemCallRequest{
        .Number = static_cast<Foundation::UInt64>(registers.Rax),
        .Arguments =
            {
                static_cast<Foundation::UInt64>(registers.Rdi),
                static_cast<Foundation::UInt64>(registers.Rsi),
                static_cast<Foundation::UInt64>(registers.Rdx),
                static_cast<Foundation::UInt64>(registers.R10),
                static_cast<Foundation::UInt64>(registers.R8),
                static_cast<Foundation::UInt64>(registers.R9),
            },
    };
}

void Amd64SystemCallRegisterCodec::ApplyResponse(const Kernel::System::SystemCallResponse& response,
                                                 Amd64SystemCallRegisterImage& registers) noexcept
{
    registers.Rax = static_cast<Foundation::UInt64>(response.Error);
    registers.Rdx = static_cast<Foundation::UInt64>(response.Value0);
    registers.R8 = static_cast<Foundation::UInt64>(response.Value1);
}
} // namespace Asteria::Platform::Amd64::System
