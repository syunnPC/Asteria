// SPDX-License-Identifier: Apache-2.0

// The user system-call break immediate for
// User/Platform/Ia64/System/Ia64SystemCallTransport.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

extern "C" void AsteriaIa64SystemCallTransportAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE,
                       Asteria::User::Platform::Ia64::System::Ia64SystemCallBreakImmediate);
}
