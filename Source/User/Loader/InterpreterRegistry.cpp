// SPDX-License-Identifier: Apache-2.0

#include "User/Loader/InterpreterRegistry.hpp"

namespace Asteria::User::Loader
{
namespace
{
[[nodiscard]] bool Names(const InterpreterMatch match, const InterpreterMatch field) noexcept
{
    return (static_cast<Foundation::UInt8>(match) & static_cast<Foundation::UInt8>(field)) != 0U;
}

// How many fields entry names, or -1 when one of them differs from
// identity's.
[[nodiscard]] Foundation::Int32 Specificity(const InterpreterEntry& entry, const ExecutableIdentity& identity) noexcept
{
    Foundation::Int32 named{0};
    if (Names(entry.Match, InterpreterMatch::Format))
    {
        if (entry.Format != identity.Format)
        {
            return -1;
        }
        ++named;
    }
    if (Names(entry.Match, InterpreterMatch::Machine))
    {
        if (entry.Machine != identity.Machine)
        {
            return -1;
        }
        ++named;
    }
    if (Names(entry.Match, InterpreterMatch::Abi))
    {
        if (entry.Abi != identity.Abi)
        {
            return -1;
        }
        ++named;
    }
    return named;
}
} // namespace

LoaderError ResolveInterpreter(const InterpreterEntry* const entries, const Foundation::Size count,
                               const ExecutableIdentity& identity, const ExecutableMachine native,
                               const char*& interpreter) noexcept
{
    const InterpreterEntry* best{nullptr};
    Foundation::Int32 bestSpecificity{-1};
    for (Foundation::Size index{0U}; (entries != nullptr) && (index < count); ++index)
    {
        const Foundation::Int32 specificity = Specificity(entries[index], identity);
        if (specificity > bestSpecificity)
        {
            best = &entries[index];
            bestSpecificity = specificity;
        }
    }
    if (best == nullptr)
    {
        return (identity.Machine == native) ? LoaderError::UnsupportedFormat : LoaderError::ForeignMachineUnsupported;
    }
    if (best->Deny || (best->Interpreter == nullptr))
    {
        return LoaderError::Denied;
    }
    interpreter = best->Interpreter;
    return LoaderError::None;
}
} // namespace Asteria::User::Loader
