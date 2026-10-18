// SPDX-License-Identifier: Apache-2.0
#include <cstring>

#include "User/Loader/InterpreterRegistry.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using User::Loader::ExecutableAbi;
using User::Loader::ExecutableByteOrder;
using User::Loader::ExecutableFormat;
using User::Loader::ExecutableIdentity;
using User::Loader::ExecutableKind;
using User::Loader::ExecutableMachine;
using User::Loader::InterpreterEntry;
using User::Loader::InterpreterMatch;
using User::Loader::LoaderError;
using User::Loader::ResolveInterpreter;

[[nodiscard]] constexpr ExecutableIdentity Identity(const ExecutableFormat format, const ExecutableMachine machine,
                                                    const ExecutableAbi abi = ExecutableAbi::Asteria) noexcept
{
    return ExecutableIdentity{.Format = format,
                              .Machine = machine,
                              .ByteOrder = ExecutableByteOrder::Little,
                              .Abi = abi,
                              .Kind = ExecutableKind::Fixed,
                              .MachineCode = 0U,
                              .AbiCode = 0U};
}

// Any IA-64 executable to one interpreter, IA-64 ELF to a better one listed
// later, AArch64 refused, then two equally specific entries for IA-32.
constexpr InterpreterEntry Entries[]{
    {.Match = InterpreterMatch::Machine,
     .Format = ExecutableFormat::Unknown,
     .Machine = ExecutableMachine::Ia64,
     .Abi = ExecutableAbi::Asteria,
     .Interpreter = "ia64-any",
     .Deny = false},
    {.Match = InterpreterMatch::Format | InterpreterMatch::Machine,
     .Format = ExecutableFormat::Elf64,
     .Machine = ExecutableMachine::Ia64,
     .Abi = ExecutableAbi::Asteria,
     .Interpreter = "ia64-elf",
     .Deny = false},
    {.Match = InterpreterMatch::Machine,
     .Format = ExecutableFormat::Unknown,
     .Machine = ExecutableMachine::AArch64,
     .Abi = ExecutableAbi::Asteria,
     .Interpreter = nullptr,
     .Deny = true},
    {.Match = InterpreterMatch::Machine,
     .Format = ExecutableFormat::Unknown,
     .Machine = ExecutableMachine::Ia32,
     .Abi = ExecutableAbi::Asteria,
     .Interpreter = "ia32-first",
     .Deny = false},
    {.Match = InterpreterMatch::Machine,
     .Format = ExecutableFormat::Unknown,
     .Machine = ExecutableMachine::Ia32,
     .Abi = ExecutableAbi::Asteria,
     .Interpreter = "ia32-second",
     .Deny = false},
    {.Match = InterpreterMatch::Format | InterpreterMatch::Machine | InterpreterMatch::Abi,
     .Format = ExecutableFormat::Pe32Plus,
     .Machine = ExecutableMachine::Amd64,
     .Abi = ExecutableAbi::Foreign,
     .Interpreter = "pe-amd64",
     .Deny = false},
};
constexpr Foundation::Size EntryCount{sizeof(Entries) / sizeof(Entries[0U])};

[[nodiscard]] bool Resolves(const ExecutableIdentity& identity, const char* const expected) noexcept
{
    const char* interpreter{nullptr};
    return (ResolveInterpreter(Entries, EntryCount, identity, ExecutableMachine::Amd64, interpreter) ==
            LoaderError::None) &&
           (interpreter != nullptr) && (std::strcmp(interpreter, expected) == 0);
}

[[nodiscard]] LoaderError Refusal(const ExecutableIdentity& identity, const InterpreterEntry* const entries,
                                  const Foundation::Size count) noexcept
{
    const char* interpreter{nullptr};
    return ResolveInterpreter(entries, count, identity, ExecutableMachine::Amd64, interpreter);
}

// The most specific entry wins whatever its place, the earlier on a tie; IA-64 ELF but not IA-64 PE
// goes to the ELF one.
void TestSpecificity(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(
        runner, "the most specific entry wins, the earlier of equals",
        Resolves(Identity(ExecutableFormat::Elf64, ExecutableMachine::Ia64), "ia64-elf"),
        Resolves(Identity(ExecutableFormat::Pe32Plus, ExecutableMachine::Ia64, ExecutableAbi::Foreign), "ia64-any"),
        Resolves(Identity(ExecutableFormat::Elf32, ExecutableMachine::Ia32), "ia32-first"),
        Resolves(Identity(ExecutableFormat::Pe32Plus, ExecutableMachine::Amd64, ExecutableAbi::Foreign), "pe-amd64"));
}

// A denying entry refuses; without a match, an image of the native machine
// is an unsupported format and another machine's a foreign machine.
void TestRefusals(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "a denied machine, and unmatched native and foreign images, are refused distinctly",
                   Refusal(Identity(ExecutableFormat::Elf64, ExecutableMachine::AArch64), Entries, EntryCount) ==
                       LoaderError::Denied,
                   Refusal(Identity(ExecutableFormat::Pe32Plus, ExecutableMachine::Amd64, ExecutableAbi::Foreign),
                           Entries, 5U) == LoaderError::UnsupportedFormat,
                   Refusal(Identity(ExecutableFormat::Elf64, ExecutableMachine::Unknown), Entries, EntryCount) ==
                       LoaderError::ForeignMachineUnsupported,
                   Refusal(Identity(ExecutableFormat::Elf64, ExecutableMachine::Ia64), nullptr, 0U) ==
                       LoaderError::ForeignMachineUnsupported);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"InterpreterRegistryTests"};
    Asteria::Test::TestSpecificity(runner);
    Asteria::Test::TestRefusals(runner);
    return runner.Finish();
}
