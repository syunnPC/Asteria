// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_LOADER_INTERPRETER_REGISTRY_HPP_INCLUDED
#define ASTERIA_USER_LOADER_INTERPRETER_REGISTRY_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Loader/ExecutableImage.hpp"

namespace Asteria::User::Loader
{
/// The fields an interpreter entry matches; a field it does not name
/// matches any value.
enum class InterpreterMatch : Foundation::UInt8
{
    Any = 0U,
    Format = 1U << 0U,
    Machine = 1U << 1U,
    Abi = 1U << 2U,
};

[[nodiscard]] constexpr InterpreterMatch operator|(const InterpreterMatch left, const InterpreterMatch right) noexcept
{
    return static_cast<InterpreterMatch>(static_cast<Foundation::UInt8>(left) | static_cast<Foundation::UInt8>(right));
}

/// One registration: the executables it covers, by the fields Match names, and the image of the
/// native interpreter that runs them, by its name in the image table; or, with Deny, a refusal of
/// what it covers.
struct InterpreterEntry
{
    InterpreterMatch Match;
    ExecutableFormat Format;
    ExecutableMachine Machine;
    ExecutableAbi Abi;
    const char* Interpreter;
    bool Deny;
};

/// Chooses the interpreter for an executable of identity that the native
/// machine cannot run itself. The entry matching the most fields wins, the
/// earlier on a tie. None with interpreter set to its image's name; Denied
/// when that entry denies; without a match, UnsupportedFormat for an image
/// of the native machine (another ABI or format) and
/// ForeignMachineUnsupported for another machine's.
[[nodiscard]] LoaderError ResolveInterpreter(const InterpreterEntry* entries, Foundation::Size count,
                                             const ExecutableIdentity& identity, ExecutableMachine native,
                                             const char*& interpreter) noexcept;
} // Namespace Asteria::User::Loader

#endif // ASTERIA_USER_LOADER_INTERPRETER_REGISTRY_HPP_INCLUDED
