// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_INPUT_BROKER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_INPUT_BROKER_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// Hardware-free Input Broker, serialized by its Manage WaitSet. Its own
/// domain mints bounded sealed source/client Endpoints; incarnation is epoch.
[[nodiscard]] Foundation::UInt64 ServeInput(const System::SystemCallClient& client,
                                            const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept;
/// Small event-consuming client used by the root's scripted demonstration.
/// Registers itself, renews one credit, and answers Query with received count.
[[nodiscard]] Foundation::UInt64
ServeInputClient(const System::SystemCallClient& client,
                 const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept;
}
#endif // ASTERIA_USER_COMPONENT_INPUT_BROKER_HPP_INCLUDED
