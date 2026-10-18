// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell
{
using Detail::Decimal;
using Detail::Is;
using Detail::Name;
using Detail::Quoted;
using Detail::Slice;
using Detail::SpecialBuiltin;
using Detail::Validate;
Engine::Engine(const Filesystem filesystem, const ProcessHost host) noexcept : m_filesystem{filesystem}, m_host{host}
{
    Environment.Name.Bytes[0U] = 's';
    Environment.Name.Bytes[1U] = 'h';
    Environment.Name.Length = 2U;
}
bool Engine::Fail(const Error error, const Size offset) noexcept
{
    if (m_error.Ok())
    {
        m_error = {error, offset, 2};
    }
    return false;
}
bool Engine::Step() noexcept
{
    if (m_steps >= StepLimit)
    {
        return Fail(Error::Steps);
    }
    ++m_steps;
    return m_error.Ok();
}
bool Engine::ValidateState() noexcept
{
    if (Environment.ParameterCount > FieldLimit)
    {
        return Fail(Error::FieldCapacity);
    }
    if (Environment.Name.Length > ValueLimit)
    {
        return Fail(Error::ValueCapacity);
    }
    const Result name = Validate({Environment.Name.Bytes, Environment.Name.Length});
    if (!name.Ok())
    {
        return Fail(name.Code);
    }
    for (Size i{0U}; i < Environment.ParameterCount; ++i)
    {
        const Value& value = Environment.Parameters[i];
        if (value.Length > ValueLimit)
        {
            return Fail(Error::ValueCapacity);
        }
        const Result valid = Validate({value.Bytes, value.Length});
        if (!valid.Ok())
        {
            return Fail(valid.Code);
        }
    }
    for (const Variable& variable : Environment.Variables)
    {
        if (!variable.Present)
        {
            continue;
        }
        if (variable.NameLength > NameLimit || !Name({variable.Name, variable.NameLength}))
        {
            return Fail(Error::InvalidName);
        }
        if (variable.ValueLength > ValueLimit)
        {
            return Fail(Error::ValueCapacity);
        }
        const Result valid = Validate({variable.Value, variable.ValueLength});
        if (!valid.Ok())
        {
            return Fail(valid.Code);
        }
    }
    return true;
}
Size Engine::Find(const Text name) const noexcept
{
    for (Size i{0U}; i < VariableLimit; ++i)
    {
        const Variable& v = Environment.Variables[i];
        if (v.Present && v.NameLength <= NameLimit && v.ValueLength <= ValueLimit &&
            Equal(name, {v.Name, v.NameLength}))
        {
            return i;
        }
    }
    return VariableLimit;
}
Result Engine::Set(const Text name, const Text value, const bool exported) noexcept
{
    if (name.Length > NameLimit || !Name(name))
    {
        return {Error::InvalidName, 0U, 2};
    }
    if (value.Length > ValueLimit)
    {
        return {Error::ValueCapacity, 0U, 2};
    }
    const Result valid = Validate(value);
    if (!valid.Ok())
    {
        return valid;
    }
    Size index = Find(name);
    if (index == VariableLimit)
    {
        for (Size i{0U}; i < VariableLimit; ++i)
        {
            if (!Environment.Variables[i].Present)
            {
                index = i;
                break;
            }
        }
    }
    if (index == VariableLimit)
    {
        return {Error::VariableCapacity, 0U, 2};
    }
    Variable& variable = Environment.Variables[index];
    for (Size i{0U}; i < name.Length; ++i)
    {
        variable.Name[i] = name.Data[i];
    }
    // value can be a view of this same variable; forward copy is intentional.
    for (Size i{0U}; i < value.Length; ++i)
    {
        variable.Value[i] = value.Data[i];
    }
    variable.Name[name.Length] = '\0';
    variable.Value[value.Length] = '\0';
    variable.NameLength = static_cast<UInt16>(name.Length);
    variable.ValueLength = static_cast<UInt16>(value.Length);
    variable.Present = true;
    variable.NoValue = false;
    variable.Exported = variable.Exported || exported || Environment.AllExport;
    return {};
}
Text Engine::Get(const Text name, bool& present) const noexcept
{
    const Size index = Find(name);
    present = index != VariableLimit && !Environment.Variables[index].NoValue;
    return present ? Text{Environment.Variables[index].Value, Environment.Variables[index].ValueLength} : Text{};
}
Result Engine::Unset(const Text name) noexcept
{
    if (name.Length > NameLimit || !Name(name))
    {
        return {Error::InvalidName, 0U, 2};
    }
    const Size index = Find(name);
    if (index != VariableLimit)
    {
        Environment.Variables[index].Present = false;
        Environment.Variables[index].Exported = false;
    }
    return {};
}
void Engine::CopyStateFrom(const Engine& parent) noexcept
{
    Environment = parent.Environment;
    for (Size i{0U}; i < FunctionLimit; ++i)
    {
        m_functions[i] = parent.m_functions[i];
    }
    m_steps = parent.m_steps;
    m_suppression = parent.m_suppression;
    m_functionDepth = parent.m_functionDepth;
    m_sourceDepth = parent.m_sourceDepth;
    m_childDepth = parent.m_childDepth;
}
bool Engine::Write(const Text text, const UInt16 fd) noexcept
{
    return (m_host.Write != nullptr && m_host.Write(m_host.Context, fd, text)) || Fail(Error::Host);
}
int Engine::Nested(const Text source) noexcept
{
    if (m_scriptDepth >= ScriptLimit)
    {
        static_cast<void>(Fail(Error::Depth));
        return 2;
    }
    Program& program = m_programs[m_scriptDepth++];
    const Result parsed = Parse(source, program);
    int status{2};
    if (!parsed.Ok())
    {
        static_cast<void>(Fail(parsed.Code, parsed.Offset));
    }
    else
    {
        status = Environment.NoExecute ? 0 : Evaluate(program, program.Root);
    }
    --m_scriptDepth;
    return status;
}
Result Engine::Run(const Text source) noexcept
{
    const bool top = m_executionDepth == 0U && m_scriptDepth == 0U;
    if (top)
    {
        m_error = {};
        m_steps = 0U;
        m_return = false;
        if (!ValidateState())
        {
            return m_error;
        }
    }
    const int status = Nested(source);
    Environment.LastStatus = status;
    Result result = m_error;
    result.Status = status;
    return result;
}
Result Engine::Execute(const Program& program, const UInt16 node) noexcept
{
    const bool top = m_executionDepth == 0U;
    if (top)
    {
        m_error = {};
        m_steps = 0U;
        m_return = false;
        if (!ValidateState())
        {
            return m_error;
        }
    }
    if (!program.Diagnostic.Ok() || program.NodeCount > NodeLimit || program.WordCount > WordLimit ||
        program.RedirectCount > RedirectLimit || program.Length > InputLimit ||
        program.StorageLength > SourceStorageLimit || node > program.NodeCount || program.Root > program.NodeCount)
    {
        return {Error::InvalidProgram, 0U, 2};
    }
    // Public AST is inspectable, but corrupted array links are never trusted.
    for (Size i{1U}; i <= program.NodeCount; ++i)
    {
        const Node& n = program.Nodes[i];
        if (static_cast<unsigned>(n.Kind) > static_cast<unsigned>(NodeKind::Negate) ||
            static_cast<Size>(n.Source.Offset) + n.Source.Length > program.Length ||
            ((n.Kind == NodeKind::For || n.Kind == NodeKind::Case || n.Kind == NodeKind::CaseItem ||
              n.Kind == NodeKind::Function) &&
             n.Words == 0U) ||
            (n.Kind == NodeKind::Function && n.Left == 0U) || n.Left > program.NodeCount ||
            n.Right > program.NodeCount || n.Third > program.NodeCount || n.Next > program.NodeCount ||
            n.Words > program.WordCount || n.Redirects > program.RedirectCount)
        {
            return {Error::InvalidProgram, 0U, 2};
        }
    }
    for (Size i{1U}; i <= program.WordCount; ++i)
    {
        const Word& w = program.Words[i];
        if (w.Next > program.WordCount ||
            static_cast<Size>(w.Spelling.Offset) + w.Spelling.Length > program.StorageLength)
        {
            return {Error::InvalidProgram, 0U, 2};
        }
    }
    for (Size i{1U}; i <= program.RedirectCount; ++i)
    {
        const Redirect& r = program.Redirects[i];
        if (r.Next > program.RedirectCount ||
            static_cast<Size>(r.Target.Offset) + r.Target.Length > program.StorageLength ||
            static_cast<Size>(r.Body.Offset) + r.Body.Length > program.Length)
        {
            return {Error::InvalidProgram, 0U, 2};
        }
    }
    const int status = Evaluate(program, node == 0U ? program.Root : node);
    Environment.LastStatus = status;
    Result result = m_error;
    result.Status = status;
    return result;
}
bool Engine::SaveAssignment(ExecutionScratch& frame, const Text name) noexcept
{
    Size index = Find(name);
    if (index == VariableLimit)
    {
        for (Size i{0U}; i < VariableLimit; ++i)
        {
            if (!Environment.Variables[i].Present)
            {
                index = i;
                break;
            }
        }
    }
    if (index == VariableLimit)
    {
        return Fail(Error::VariableCapacity);
    }
    for (Size i{0U}; i < frame.SavedCount; ++i)
    {
        if (frame.SavedIndices[i] == index)
        {
            return true;
        }
    }
    if (frame.SavedCount >= AssignmentLimit)
    {
        return Fail(Error::VariableCapacity);
    }
    frame.SavedIndices[frame.SavedCount] = index;
    frame.Saved[frame.SavedCount++] = Environment.Variables[index];
    return true;
}
void Engine::RestoreAssignments(ExecutionScratch& frame) noexcept
{
    for (Size i{0U}; i < frame.SavedCount; ++i)
    {
        Environment.Variables[frame.SavedIndices[i]] = frame.Saved[i];
    }
}
bool Engine::Redirects(const Program& program, UInt16 first, ExecutionScratch& frame, UInt64& cookie) noexcept
{
    Size count{0U};
    frame.Targets.Count = 0U;
    frame.Targets.Used = 0U;
    while (first != 0U && Step())
    {
        if (count >= CommandRedirectLimit)
        {
            return Fail(Error::RedirectCapacity);
        }
        const Redirect& redirect = program.Redirects[first];
        const Size targetIndex = frame.Targets.Count;
        if (redirect.Kind == TokenKind::Here || redirect.Kind == TokenKind::HereStrip)
        {
            const Text body = program.View(redirect.Body);
            Buffer stripped{frame.TextBytes, InputLimit};
            bool beginning{true};
            for (Size i{0U}; i < body.Length; ++i)
            {
                if (!redirect.Quoted && body.Data[i] == '\\' && i + 1U < body.Length)
                {
                    if (body.Data[i + 1U] == '\n')
                    {
                        ++i;
                        continue;
                    }
                    if (body.Data[i + 1U] == '\\')
                    {
                        if (!stripped.Append(Slice(body, i, 2U)))
                        {
                            return Fail(Error::ExpansionCapacity);
                        }
                        ++i;
                        beginning = false;
                        continue;
                    }
                }
                if (beginning && redirect.Kind == TokenKind::HereStrip && body.Data[i] == '\t')
                {
                    continue;
                }
                if (!stripped.Put(body.Data[i]))
                {
                    return Fail(Error::ExpansionCapacity);
                }
                beginning = body.Data[i] == '\n';
            }
            if (redirect.Quoted)
            {
                if (!frame.Targets.Add(stripped.View()))
                {
                    return Fail(Error::ExpansionCapacity);
                }
            }
            else
            {
                if (m_expansionDepth >= ScriptLimit)
                {
                    return Fail(Error::Depth);
                }
                ExpansionScratch& out = m_expansions[m_expansionDepth++];
                out.Length = 0U;
                out.KeepEmpty = false;
                out.BoundaryCount = 0U;
                out.MarkerCount = 0U;
                const bool ok =
                    ExpandInto(stripped.View(), out, true, true) && frame.Targets.Add({out.Bytes, out.Length});
                --m_expansionDepth;
                if (!ok)
                {
                    return m_error.Ok() ? Fail(Error::ExpansionCapacity) : false;
                }
            }
        }
        else
        {
            const Result expanded = Expand(program.View(redirect.Target), frame.Targets, false, false);
            if (!expanded.Ok())
            {
                return Fail(expanded.Code, expanded.Offset);
            }
            if (frame.Targets.Count != targetIndex + 1U)
            {
                return Fail(Error::Syntax);
            }
        }
        const Text target = frame.Targets.At(targetIndex);
        if (redirect.Kind == TokenKind::LessAnd || redirect.Kind == TokenKind::GreatAnd)
        {
            UInt64 fd{0U};
            if (!Is(target, "-") && (!Decimal(target, fd) || fd > 65535U))
            {
                return Fail(Error::Syntax);
            }
        }
        frame.Redirects[count++] = {redirect.Kind, redirect.Descriptor, target,
                                    Environment.NoClobber && redirect.Kind == TokenKind::Great};
        first = redirect.Next;
    }
    if (!m_error.Ok())
    {
        return false;
    }
    if (count == 0U)
    {
        cookie = 0U;
        return true;
    }
    return (m_host.BeginRedirects != nullptr && m_host.EndRedirects != nullptr &&
            m_host.BeginRedirects(m_host.Context, frame.Redirects, count, cookie)) ||
           Fail(Error::Host);
}
int Engine::Invoke(ExecutionScratch& frame) noexcept
{
    int status{0};
    const Text name = frame.Arguments.At(0U);
    Function* function{nullptr};
    if (!SpecialBuiltin(name))
    {
        for (Size i{0U}; i < FunctionLimit; ++i)
        {
            if (m_functions[i].Present && Equal(name, {m_functions[i].Name, m_functions[i].NameLength}))
            {
                function = &m_functions[i];
                break;
            }
        }
    }
    if (function != nullptr)
    {
        frame.ParameterCount = Environment.ParameterCount;
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            frame.Parameters[i] = Environment.Parameters[i];
        }
        Environment.ParameterCount = frame.Arguments.Count - 1U;
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            const Text parameter = frame.Arguments.At(i + 1U);
            if (parameter.Length > ValueLimit)
            {
                static_cast<void>(Fail(Error::ValueCapacity));
                break;
            }
            for (Size j{0U}; j < parameter.Length; ++j)
            {
                Environment.Parameters[i].Bytes[j] = parameter.Data[j];
            }
            Environment.Parameters[i].Length = static_cast<UInt16>(parameter.Length);
        }
        // A function may redefine itself: copy its source before evaluation.
        for (Size i{0U}; i < function->Length; ++i)
        {
            frame.TextBytes[i] = function->Source[i];
        }
        const Size sourceLength = function->Length;
        ++m_functionDepth;
        status = m_error.Ok() ? Nested({frame.TextBytes, sourceLength}) : 2;
        --m_functionDepth;
        m_return = false;
        Environment.ParameterCount = frame.ParameterCount;
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            Environment.Parameters[i] = frame.Parameters[i];
        }
    }
    else
    {
        bool found{false};
        status = Builtin(frame.Arguments, frame, found);
        if (!found)
        {
            if (m_host.Launch == nullptr || !m_host.Launch(m_host.Context, frame.Arguments, Environment, status))
            {
                static_cast<void>(Fail(Error::Host));
                status = 127;
            }
        }
    }
    return status;
}
int Engine::Simple(const Program& program, const Node& node, ExecutionScratch& frame) noexcept
{
    frame.Arguments.Count = 0U;
    frame.Arguments.Used = 0U;
    frame.SavedCount = 0U;
    for (UInt16 w = node.Words; w != 0U && Step(); w = program.Words[w].Next)
    {
        if (!program.Words[w].Assignment)
        {
            const Text word = program.View(program.Words[w].Spelling);
            Size equal{0U};
            while (equal < word.Length && word.Data[equal] != '=')
            {
                ++equal;
            }
            if (frame.Arguments.Count != 0U && Is(frame.Arguments.At(0U), "export") && equal < word.Length &&
                Name(Slice(word, 0U, equal)))
            {
                frame.Targets.Count = 0U;
                frame.Targets.Used = 0U;
                if (!Expand(Slice(word, equal + 1U, word.Length - equal - 1U), frame.Targets, false, false, true).Ok())
                {
                    return 2;
                }
                Buffer assignment{frame.TextBytes, InputLimit};
                if (!assignment.Append(Slice(word, 0U, equal + 1U)) || !assignment.Append(frame.Targets.At(0U)) ||
                    !frame.Arguments.Add(assignment.View()))
                {
                    static_cast<void>(Fail(Error::ExpansionCapacity));
                    return 2;
                }
            }
            else
            {
                const Result result = Expand(word, frame.Arguments);
                if (!result.Ok())
                {
                    return 2;
                }
            }
        }
    }
    if (Environment.Trace && frame.Arguments.Count != 0U)
    {
        if (!Write(Literal("+ "), 2U))
        {
            return 2;
        }
        for (Size i{0U}; i < frame.Arguments.Count; ++i)
        {
            if ((i != 0U && !Write(Literal(" "), 2U)) || !Write(frame.Arguments.At(i), 2U))
            {
                return 2;
            }
        }
        if (!Write(Literal("\n"), 2U))
        {
            return 2;
        }
    }
    const bool persistent = frame.Arguments.Count == 0U || SpecialBuiltin(frame.Arguments.At(0U));
    UInt64 cookie{0U};
    if (!Redirects(program, node.Redirects, frame, cookie))
    {
        return 2;
    }
    int status{0};
    for (UInt16 w = node.Words; w != 0U && Step(); w = program.Words[w].Next)
    {
        if (!program.Words[w].Assignment)
        {
            continue;
        }
        const Text word = program.View(program.Words[w].Spelling);
        Size equal{0U};
        while (equal < word.Length && word.Data[equal] != '=')
        {
            ++equal;
        }
        const Text name = Slice(word, 0U, equal);
        if (!persistent && !SaveAssignment(frame, name))
        {
            break;
        }
        frame.Targets.Count = 0U;
        frame.Targets.Used = 0U;
        if (!Expand(Slice(word, equal + 1U, word.Length - equal - 1U), frame.Targets, false, false, true).Ok())
        {
            break;
        }
        const Result set = Set(name, frame.Targets.At(0U), !persistent);
        if (!set.Ok())
        {
            static_cast<void>(Fail(set.Code));
            break;
        }
    }
    if (!m_error.Ok())
    {
        status = 2;
    }
    else if (frame.Arguments.Count == 0U)
    {
        status = frame.HasSubstitution ? frame.SubstitutionStatus : 0;
    }
    else
    {
        status = Invoke(frame);
    }
    if (node.Redirects != 0U)
    {
        m_host.EndRedirects(m_host.Context, cookie);
    }
    if (!persistent)
    {
        RestoreAssignments(frame);
    }
    return status;
}
int Engine::Evaluate(const Program& program, const UInt16 index) noexcept
{
    if (index == 0U || Environment.NoExecute)
    {
        return 0;
    }
    if (!Step() || Environment.ExitRequested || m_return)
    {
        return Environment.LastStatus;
    }
    if (m_executionDepth >= DepthLimit)
    {
        static_cast<void>(Fail(Error::Depth));
        return 2;
    }
    ExecutionScratch& frame = m_execution[m_executionDepth++];
    frame.HasSubstitution = false;
    frame.SubstitutionStatus = 0;
    const Node& node = program.Nodes[index];
    int status{0};
    UInt64 cookie{0U};
    bool redirected{false};
    if (node.Kind != NodeKind::Simple && node.Kind != NodeKind::Function && node.Redirects != 0U)
    {
        redirected = Redirects(program, node.Redirects, frame, cookie);
        if (!redirected)
        {
            --m_executionDepth;
            return 2;
        }
    }
    switch (node.Kind)
    {
    case NodeKind::Simple:
        status = Simple(program, node, frame);
        break;
    case NodeKind::List:
        for (UInt16 n = node.Left; n != 0U && Step() && !Environment.ExitRequested && !m_return;
             n = program.Nodes[n].Next)
        {
            status = Evaluate(program, n);
            Environment.LastStatus = status;
        }
        break;
    case NodeKind::And:
    case NodeKind::Or:
        ++m_suppression;
        status = Evaluate(program, node.Left);
        --m_suppression;
        Environment.LastStatus = status;
        if (m_error.Ok() && !Environment.ExitRequested && !m_return &&
            (node.Kind == NodeKind::And ? status == 0 : status != 0))
        {
            status = Evaluate(program, node.Right);
        }
        break;
    case NodeKind::Negate:
        ++m_suppression;
        status = Evaluate(program, node.Left) == 0 ? 1 : 0;
        --m_suppression;
        break;
    case NodeKind::Group:
        status = Evaluate(program, node.Left);
        break;
    case NodeKind::Subshell:
    case NodeKind::Pipeline:
    case NodeKind::Background:
    {
        if (m_childDepth + 1U >= ScriptLimit)
        {
            static_cast<void>(Fail(Error::Depth));
            status = 2;
            break;
        }
        UInt64 pid{0U};
        const Isolation kind = node.Kind == NodeKind::Subshell
                                   ? Isolation::Subshell
                                   : (node.Kind == NodeKind::Pipeline ? Isolation::Pipeline : Isolation::Background);
        if (kind == Isolation::Pipeline)
        {
            ++m_suppression;
        }
        ++m_childDepth;
        if (m_host.Isolate == nullptr ||
            !m_host.Isolate(m_host.Context, kind, *this, program, node.Kind == NodeKind::Pipeline ? index : node.Left,
                            status, pid))
        {
            static_cast<void>(Fail(Error::Host));
            status = 2;
        }
        else if (node.Kind == NodeKind::Background)
        {
            Environment.BackgroundId = pid;
            status = 0;
        }
        --m_childDepth;
        if (kind == Isolation::Pipeline)
        {
            --m_suppression;
        }
        break;
    }
    case NodeKind::If:
        ++m_suppression;
        status = Evaluate(program, node.Left);
        --m_suppression;
        Environment.LastStatus = status;
        if (m_error.Ok() && !Environment.ExitRequested && !m_return)
        {
            status = Evaluate(program, status == 0 ? node.Right : node.Third);
        }
        break;
    case NodeKind::While:
    case NodeKind::Until:
    {
        int last{0};
        while (Step() && !Environment.ExitRequested && !m_return)
        {
            ++m_suppression;
            const int condition = Evaluate(program, node.Left);
            --m_suppression;
            Environment.LastStatus = condition;
            if (!m_error.Ok() || Environment.ExitRequested || m_return ||
                (node.Kind == NodeKind::While ? condition != 0 : condition == 0))
            {
                break;
            }
            last = Evaluate(program, node.Right);
        }
        status = last;
        break;
    }
    case NodeKind::For:
    {
        frame.Arguments.Count = 0U;
        frame.Arguments.Used = 0U;
        if (node.ImplicitParameters)
        {
            for (Size i{0U}; i < Environment.ParameterCount; ++i)
            {
                if (!frame.Arguments.Add({Environment.Parameters[i].Bytes, Environment.Parameters[i].Length}))
                {
                    static_cast<void>(Fail(Error::ExpansionCapacity));
                    break;
                }
            }
        }
        else
        {
            for (UInt16 w = program.Words[node.Words].Next; w != 0U && Step(); w = program.Words[w].Next)
            {
                if (!Expand(program.View(program.Words[w].Spelling), frame.Arguments).Ok())
                {
                    break;
                }
            }
        }
        for (Size i{0U}; i < frame.Arguments.Count && Step() && !Environment.ExitRequested && !m_return; ++i)
        {
            const Result set = Set(program.View(program.Words[node.Words].Spelling), frame.Arguments.At(i));
            if (!set.Ok())
            {
                static_cast<void>(Fail(set.Code));
                break;
            }
            status = Evaluate(program, node.Left);
        }
        break;
    }
    case NodeKind::Case:
    {
        frame.Arguments.Count = 0U;
        frame.Arguments.Used = 0U;
        if (!Expand(program.View(program.Words[node.Words].Spelling), frame.Arguments, false, false).Ok())
        {
            break;
        }
        bool matched{false};
        bool fallthrough{false};
        for (UInt16 item = node.Left; item != 0U && !matched && Step(); item = program.Nodes[item].Next)
        {
            matched = fallthrough;
            for (UInt16 w = program.Nodes[item].Words; w != 0U && !matched && Step(); w = program.Words[w].Next)
            {
                if (m_expansionDepth >= ScriptLimit)
                {
                    static_cast<void>(Fail(Error::Depth));
                    break;
                }
                ExpansionScratch& pattern = m_expansions[m_expansionDepth++];
                pattern.Length = 0U;
                pattern.KeepEmpty = false;
                pattern.BoundaryCount = 0U;
                pattern.MarkerCount = 0U;
                matched = ExpandInto(program.View(program.Words[w].Spelling), pattern, false) &&
                          Match({pattern.Bytes, pattern.Length}, frame.Arguments.At(0U), pattern.Flags);
                --m_expansionDepth;
            }
            if (matched)
            {
                status = Evaluate(program, program.Nodes[item].Left);
                fallthrough = program.Nodes[item].ImplicitParameters;
                if (fallthrough)
                {
                    matched = false;
                }
            }
        }
        break;
    }
    case NodeKind::Function:
    {
        const Text name = program.View(program.Words[node.Words].Spelling);
        Size chosen{FunctionLimit};
        for (Size i{0U}; i < FunctionLimit; ++i)
        {
            if (m_functions[i].Present && Equal(name, {m_functions[i].Name, m_functions[i].NameLength}))
            {
                chosen = i;
                break;
            }
        }
        if (chosen == FunctionLimit)
        {
            for (Size i{0U}; i < FunctionLimit; ++i)
            {
                if (!m_functions[i].Present)
                {
                    chosen = i;
                    break;
                }
            }
        }
        if (chosen == FunctionLimit)
        {
            static_cast<void>(Fail(Error::FunctionCapacity));
            break;
        }
        if (name.Length > NameLimit || SpecialBuiltin(name))
        {
            static_cast<void>(Fail(Error::InvalidName));
            break;
        }
        Function& function = m_functions[chosen];
        const Text body = program.View(program.Nodes[node.Left].Source);
        function.NameLength = name.Length;
        function.Length = body.Length;
        function.Present = true;
        for (Size i{0U}; i < name.Length; ++i)
        {
            function.Name[i] = name.Data[i];
        }
        for (Size i{0U}; i < body.Length; ++i)
        {
            function.Source[i] = body.Data[i];
        }
        break;
    }
    case NodeKind::CaseItem:
        static_cast<void>(Fail(Error::InvalidProgram));
        status = 2;
        break;
    }
    if (redirected)
    {
        m_host.EndRedirects(m_host.Context, cookie);
    }
    --m_executionDepth;
    if (!m_error.Ok())
    {
        status = 2;
    }
    if (m_suppression == 0U && Environment.Errexit && status != 0 &&
        (node.Kind == NodeKind::Simple || node.Kind == NodeKind::Subshell || node.Kind == NodeKind::Pipeline))
    {
        Environment.ExitRequested = true;
    }
    Environment.LastStatus = status;
    return status;
}
} // namespace Asteria::User::Shell
