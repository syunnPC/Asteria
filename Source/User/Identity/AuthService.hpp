// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_AUTH_SERVICE_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_AUTH_SERVICE_HPP_INCLUDED
#include "Abi/ComponentBootstrap.hpp"
#include "User/Identity/Budget.hpp"
#include "User/Identity/SeatFrame.hpp"
namespace Asteria::User::Identity
{
using SeatClock = UInt64 (*)() noexcept;
// One statically owned incarnation; the loop and injected host transport use
// the same bounded steps. Clear follows a failed step or completed drain.
[[nodiscard]] bool InitializeAuth(const System::SystemCallClient&, const UInt64 (&)[Abi::ComponentBlockWords],
                                  SeatClock, UInt64 = 0U) noexcept;
[[nodiscard]] bool AdvanceAuth() noexcept;
void ClearAuth() noexcept;
// Static owner, funded before Ready; root's retained nonsecret limiter page survives this
// component's drain/restart.
[[nodiscard]] UInt64 ServeAuth(const System::SystemCallClient& client, const UInt64 (&block)[Abi::ComponentBlockWords],
                               SeatClock clock, UInt64 region = 0U) noexcept;
}
#endif
