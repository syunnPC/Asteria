// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"

extern "C" void AsteriaAmd64UserSystemCallTransport(void* context, const Asteria::Abi::SystemCallRequest* request,
                                                    Asteria::Abi::SystemCallResponse* response) noexcept;

// The target transport is in Amd64SystemCallTransport.S.
#if !defined(ASTERIA_TARGET_AMD64)
extern "C" void AsteriaAmd64UserSystemCallTransport(void*, const Asteria::Abi::SystemCallRequest*,
                                                    Asteria::Abi::SystemCallResponse* const response) noexcept
{
    *response = {Asteria::Abi::SystemCallError::InvalidConfiguration, 0U, 0U};
}
#endif

namespace Asteria::User::Platform::Amd64::System
{
User::System::SystemCallTransport GetAmd64SystemCallTransport() noexcept
{
    return {AsteriaAmd64UserSystemCallTransport, nullptr};
}
}
