// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

extern "C" void AsteriaIa64UserSystemCallTransport(void* context, const Asteria::Abi::SystemCallRequest* request,
                                                   Asteria::Abi::SystemCallResponse* response) noexcept;

// The target entry point is in Ia64SystemCallTransport.S.
#if !defined(__ia64__)
extern "C" void AsteriaIa64UserSystemCallTransport(void* const context,
                                                   const Asteria::Abi::SystemCallRequest* const request,
                                                   Asteria::Abi::SystemCallResponse* const response) noexcept
{
    static_cast<void>(context);
    static_cast<void>(request);
    *response = Asteria::Abi::SystemCallResponse{
        .Error = Asteria::Abi::SystemCallError::InvalidConfiguration,
        .Value0 = 0U,
        .Value1 = 0U,
    };
}
#endif

namespace Asteria::User::Platform::Ia64::System
{
User::System::SystemCallTransport GetIa64SystemCallTransport() noexcept
{
    return User::System::SystemCallTransport{
        .Function = AsteriaIa64UserSystemCallTransport,
        .Context = nullptr,
    };
}
} // namespace Asteria::User::Platform::Ia64::System
