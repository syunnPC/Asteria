// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_PS2_PUBLICATION_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_PS2_PUBLICATION_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
// Publication observation is a separate link in product and test boot profiles
// implementation; the device loop never waits for observers.
void ObservePs2Publication(const Input::InputEvent* events, Foundation::Size count) noexcept;
[[nodiscard]] bool SendPs2Publication(const System::SystemCallClient& client, System::CapabilityHandle root) noexcept;
[[nodiscard]] bool Ps2PublicationPending() noexcept;
}
#endif
