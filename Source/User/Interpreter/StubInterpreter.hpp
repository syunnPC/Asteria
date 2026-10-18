// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INTERPRETER_STUB_INTERPRETER_HPP_INCLUDED
#define ASTERIA_USER_INTERPRETER_STUB_INTERPRETER_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Loader/ExecutableImage.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Interpreter
{
/// The stub's report to the root service: an asynchronous message of this
/// interface whose payload holds five little-endian words: the guest's raw
/// machine code, its entry instruction and gp, its segment count and its
/// size in bytes.
inline constexpr Kernel::Ipc::InterfaceIdentifier StubReportInterface{0x5354'5542'0000'0001ULL,
                                                                      0x4153'5445'5249'4101ULL};
inline constexpr Foundation::UInt16 StubReportPayloadBytes{40U};
/// The codes the stub exits with: after its report, or when the launch was
/// not one it can take (no report then).
inline constexpr Foundation::UInt64 StubExitCode{Abi::ComponentInterpretedExitCode};
inline constexpr Foundation::UInt64 StubFailedExitCode{Abi::ComponentInterpretFailedExitCode};

/// The opposite machine's static executable rules, selected by the native
/// image adapter and the root service's demonstration.
inline constexpr Loader::MachineRules Ia64GuestRules{.Machine = Loader::ExecutableMachine::Ia64,
                                                     .UserStart = Abi::Ia64ComponentUserStart,
                                                     .UserLimit = Abi::Ia64ComponentUserLimit,
                                                     .EntryDescriptor = true,
                                                     .MaximumSegments = Loader::MaximumExecutableSegments,
                                                     .MaximumSegmentPages = Abi::ComponentForeignImagePages,
                                                     .WritableFileBytes = true};
inline constexpr Loader::MachineRules Amd64GuestRules{.Machine = Loader::ExecutableMachine::Amd64,
                                                      .UserStart = 0x1000U,
                                                      .UserLimit = 0x0000'8000'0000'0000ULL,
                                                      .EntryDescriptor = false,
                                                      .MaximumSegments = Loader::MaximumExecutableSegments,
                                                      .MaximumSegmentPages = Abi::ComponentForeignImagePages,
                                                      .WritableFileBytes = true};

/// What the stub works on: a component keeps it in.bss.
struct StubBuffers
{
    Kernel::Ipc::MessageEnvelope Message;
    alignas(8) Foundation::UInt8 Frame[Kernel::Ipc::MessageCodec::MaximumWireSizeBytes];
    Loader::ExecutableImage Guest;
};

/// A test interpreter for foreign executables. It emulates nothing: it requires an interpreted
/// launch (the behavior word and the extension record of block), describes the read-only guest the
/// record names under the supplied guest machine rules with the loader's own handlers, and requires
/// The identity the record gives. Then it reports the guest's description on its report Endpoint
/// (words 2 and 3). Returns the code it exits with.
[[nodiscard]] Foundation::UInt64
RunStubInterpreter(System::SystemCallTransport transport,
                   const Foundation::UInt64 (&block)[Abi::ComponentInterpreterBlockWords], StubBuffers& buffers,
                   const Loader::MachineRules& guestRules) noexcept;
} // Namespace Asteria::User::Interpreter

#endif // ASTERIA_USER_INTERPRETER_STUB_INTERPRETER_HPP_INCLUDED
