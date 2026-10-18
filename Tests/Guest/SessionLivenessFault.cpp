// SPDX-License-Identifier: Apache-2.0
#include "User/Component/SessionLiveness.hpp"
#include "Foundation/Types.hpp"
namespace Asteria::User::Component
{
// Admit once, then withhold replies for the real watchdog to retire us.
bool AnswerSessionLivenessQuery() noexcept
{
    static constinit Foundation::UInt64 queries{0U};
    return ++queries <= 1U;
}
} // namespace Asteria::User::Component
