// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Amd64/Runtime/Amd64MinimalUserImageRuntime.hpp"
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"

extern "C"
{
constinit Asteria::Abi::SystemCallResponse g_waitSystemCallResponses[2U]{};
constinit Asteria::Foundation::UInt64 g_waitCompletionSentinel{0U};
}

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64MinimalUserImageMain(
    const Asteria::Foundation::UInt64 selector, const Asteria::Foundation::UInt64 authenticator,
    const Asteria::Foundation::UInt64 waitSelector, const Asteria::Foundation::UInt64 waitAuthenticator) noexcept
{
    const auto transport = Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport();
    const auto ipc =
        Asteria::User::Platform::Amd64::Runtime::RunAmd64MinimalUserImage(selector, authenticator, transport);
    if (ipc != 0U)
    {
        return ipc;
    }
    const Asteria::User::System::SystemCallClient client{transport};
    for (Asteria::Foundation::Size sequence{0U}; sequence < 2U; ++sequence)
    {
        g_waitSystemCallResponses[sequence] = client.Wait({waitSelector, waitAuthenticator});
        const auto& response = g_waitSystemCallResponses[sequence];
        if (response.Error != Asteria::Abi::SystemCallError::None || response.Value0 != 0x5741'4954U ||
            response.Value1 != 1U)
        {
            return 0x105U;
        }
    }
    g_waitCompletionSentinel = 0x5741'4954'5041'5353ULL;
    return 0U;
}
