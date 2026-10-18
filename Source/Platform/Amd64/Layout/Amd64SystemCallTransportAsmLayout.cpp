// SPDX-License-Identifier: Apache-2.0

// SystemCallRequest and SystemCallResponse offsets for
// User/Platform/Amd64/System/Amd64SystemCallTransport.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Abi/SystemCallAbi.hpp"

namespace
{
using Asteria::Abi::SystemCallRequest;
using Asteria::Abi::SystemCallResponse;
} // namespace

extern "C" void AsteriaAmd64SystemCallTransportAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_NUMBER, __builtin_offsetof(SystemCallRequest, Number));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_0, __builtin_offsetof(SystemCallRequest, Arguments[0]));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_1, __builtin_offsetof(SystemCallRequest, Arguments[1]));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_2, __builtin_offsetof(SystemCallRequest, Arguments[2]));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_3, __builtin_offsetof(SystemCallRequest, Arguments[3]));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_4, __builtin_offsetof(SystemCallRequest, Arguments[4]));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_5, __builtin_offsetof(SystemCallRequest, Arguments[5]));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_RESPONSE_ERROR, __builtin_offsetof(SystemCallResponse, Error));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_RESPONSE_VALUE0, __builtin_offsetof(SystemCallResponse, Value0));
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_RESPONSE_VALUE1, __builtin_offsetof(SystemCallResponse, Value1));
}
