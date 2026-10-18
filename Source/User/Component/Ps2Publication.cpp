// SPDX-License-Identifier: Apache-2.0

#include "User/Component/Ps2Publication.hpp"

namespace Asteria::User::Component
{
void ObservePs2Publication(const Input::InputEvent*, Foundation::Size) noexcept
{
}
bool SendPs2Publication(const System::SystemCallClient&, System::CapabilityHandle) noexcept
{
    return true;
}
bool Ps2PublicationPending() noexcept
{
    return false;
}
} // namespace Asteria::User::Component
