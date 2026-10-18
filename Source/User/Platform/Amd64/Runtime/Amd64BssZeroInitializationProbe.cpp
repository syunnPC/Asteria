// SPDX-License-Identifier: Apache-2.0

#include "User/Platform/Amd64/Runtime/Amd64BssZeroInitializationProbe.hpp"

namespace Asteria::User::Platform::Amd64::Runtime
{
constinit volatile Foundation::UInt64 g_bssZeroInitializationProbe;

[[gnu::noinline]] void ObserveBssZeroInitialization() noexcept
{
    const Foundation::UInt64 observedValue{g_bssZeroInitializationProbe};
    if (observedValue == 0U)
    {
        g_bssZeroInitializationProbe = BssZeroInitializationPassedSentinel;
    }
    else
    {
        g_bssZeroInitializationProbe = BssZeroInitializationFailedSentinel;
    }
}
} // namespace Asteria::User::Platform::Amd64::Runtime
