// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_DISPLAY_BROKER_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_DISPLAY_BROKER_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The System Display Broker, a long-lived service of the root service. It maps the scanout its
/// start block names (word 30, the mode at words 40 and 41) at address of its own space (the
/// platform passes word 42; a hosted test passes its own buffer's, its kernel model mapping
/// nothing), through a device Frame and a WC MemoryObject with UC fallback of its own domain (words
/// 24-27), fills it through composition and serves the console protocol (DisplayFrame.hpp) on its
/// Endpoint until it is killed: it takes every message queued, ConsoleWrite into the system
/// console's cell grid, a Query answered after drawing the rows that changed. Its epoch is its
/// incarnation. Returns only a failure's exit code (Abi::ComponentDisplayFailedExitCode with its
/// step and error); blocks in WaitSetWait. The optional ISA counter uses start word 43's tick scale
/// for startup and full-console pass measurements.
[[nodiscard]] Foundation::UInt64 ServeDisplay(const System::SystemCallClient& client,
                                              const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                              Foundation::UInt64 address, ComponentClock clock = nullptr) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_DISPLAY_BROKER_HPP_INCLUDED
