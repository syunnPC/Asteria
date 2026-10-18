// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_SESSION_LIVENESS_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_SESSION_LIVENESS_HPP_INCLUDED
namespace Asteria::User::Component
{
// The fault plan is a separate link-time unit.
[[nodiscard]] bool AnswerSessionLivenessQuery() noexcept;
}
#endif
