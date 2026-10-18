// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Ia64/Runtime/Ia64MinimalUserImageRuntime.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

// The production image reports nothing through the DiagnosticChannel it
// receives in r6/r7; only a test image uses it.
extern "C" Asteria::Foundation::UInt64
AsteriaIa64MinimalUserImageMain(const Asteria::Foundation::UInt64 bootstrapSelector,
                                const Asteria::Foundation::UInt64 bootstrapAuthenticator,
                                [[maybe_unused]] const Asteria::Foundation::UInt64 diagnosticSelector,
                                [[maybe_unused]] const Asteria::Foundation::UInt64 diagnosticAuthenticator) noexcept
{
    const Asteria::User::System::SystemCallTransport transport{
        Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport()};
    return Asteria::User::Platform::Ia64::Runtime::RunIa64MinimalUserImage(bootstrapSelector, bootstrapAuthenticator,
                                                                           transport);
}

// The image entry, its idle loop and its entry descriptor are in
// Ia64MinimalUserImage.S.
