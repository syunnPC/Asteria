// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_CLIENT_CLIENT_ABI_HPP_INCLUDED
#define ASTERIA_USER_CLIENT_CLIENT_ABI_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"

namespace Asteria::User::Client
{
/// The interface the root service binds `echo` under in the Directory and the client resolves it
/// for.
inline constexpr Kernel::Ipc::InterfaceIdentifier EchoInterface{0x4543'484F'0000'0001ULL, 0x4153'5445'5249'4101ULL};
/// The tag of the client's Endpoint in its WaitSet, the echoes it asks for
/// in each round, and the codes it exits with: after its second report, or
/// when a step failed (its reports then tell which).
inline constexpr Foundation::UInt64 ClientWaitTag{0x434C'4930U};
inline constexpr Foundation::UInt64 ClientEchoes{3U};
inline constexpr Foundation::UInt64 ClientExitCode{0x0000'0000'C11E'0001ULL};
inline constexpr Foundation::UInt64 ClientFailedExitCode{0x0000'0000'C11E'00E1ULL};
/// The client's reports to the root service, one per round: an
/// Asynchronous message of this interface whose payload holds four little-endian words: whether it
/// resolved `echo` (1), how many echoes came back as sent, the service generation the Directory
/// resolved, and, in the second round, the error of its send through the capability the first round
/// resolved (zero in the first).
inline constexpr Kernel::Ipc::InterfaceIdentifier ClientReportInterface{0x434C'4945'4E54'0001ULL,
                                                                        0x4153'5445'5249'4101ULL};
inline constexpr Foundation::UInt16 ClientReportPayloadBytes{32U};
/// What the root service sends the client on its Endpoint once Echo was
/// restarted and rebound: an asynchronous message of this interface
/// and method, with no payload, that starts the second round.
inline constexpr Kernel::Ipc::InterfaceIdentifier ClientControlInterface{0x434C'4945'4E54'0002ULL,
                                                                         0x4153'5445'5249'4101ULL};
inline constexpr Foundation::UInt32 ClientGoMethod{1U};
} // Namespace Asteria::User::Client

#endif // ASTERIA_USER_CLIENT_CLIENT_ABI_HPP_INCLUDED
