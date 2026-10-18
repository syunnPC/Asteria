// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_SHELL_FAKE_HOST_HPP_INCLUDED
#define ASTERIA_TESTS_USER_SHELL_FAKE_HOST_HPP_INCLUDED
#include "User/Shell/Shell.hpp"
#include <new>
#include <cstring>
namespace Asteria::Test::ShellFixture
{
namespace S = User::Shell;
using Foundation::Size;
struct FakeHost
{
    alignas(S::Engine) unsigned char Storage[(S::ScriptLimit + 1U) * sizeof(S::Engine)]{};
    S::Engine* Engines[S::ScriptLimit + 1U]{};
    Size Depth{0U};
    char PipeBytes[S::ScriptLimit][2U][S::ExpansionLimit]{};
    char SavedCwd[S::ScriptLimit][1024U]{};
    Size SavedCwdLength[S::ScriptLimit]{};
    char OutputBytes[65536U]{};
    S::Buffer Output{OutputBytes, sizeof(OutputBytes)};
    S::Buffer* Sink{&Output};
    char ErrorBytes[8192U]{};
    S::Buffer Errors{ErrorBytes, sizeof(ErrorBytes)};
    S::Fields LaunchArguments{};
    S::State LaunchState{};
    S::Fields RedirectTargets{};
    S::TokenKind RedirectKinds[S::CommandRedirectLimit]{};
    Foundation::UInt16 RedirectFds[S::CommandRedirectLimit]{};
    Size Begins{0U};
    Size Ends{0U};
    Size Captures{0U};
    Size Launches{0U};
    Size Isolations{0U};
    bool FailLaunch{false};
    bool FailRedirect{false};
    S::Error ChildFailure{S::Error::None};
    S::Text Input{};
    Size InputCursor{0U};
    S::Text Script{S::Literal("source_value=$1; return 4; echo unreachable")};
    char CwdBytes[1024U]{'/', 'w', 'o', 'r', 'k'};
    Size CwdLength{5U};
    S::ProcessHost Host() noexcept
    {
        return {this, Launch, Write, Read, Begin, End, Isolate, Capture, Resolve, IsTerminal};
    }
    S::Filesystem Filesystem() noexcept
    {
        return {this, List, Home, Cd, Pwd, ReadFile, Inspect};
    }
    S::Engine& Reset() noexcept
    {
        Output.Length = 0U;
        Errors.Length = 0U;
        Depth = 0U;
        Sink = &Output;
        Begins = 0U;
        Ends = 0U;
        Captures = 0U;
        Launches = 0U;
        Isolations = 0U;
        FailLaunch = false;
        FailRedirect = false;
        Input = {};
        InputCursor = 0U;
        ChildFailure = S::Error::None;
        CwdLength = 5U;
        static_cast<void>(std::memcpy(CwdBytes, "/work", 5U));
        Engines[0U] = ::new (Storage) S::Engine{Filesystem(), Host()};
        static_cast<void>(Engines[0U]->Set(S::Literal("HOME"), S::Literal("/home/test")));
        static_cast<void>(Engines[0U]->Set(S::Literal("PWD"), S::Literal("/work")));
        return *Engines[0U];
    }
    S::Engine* Child(S::Engine& parent) noexcept
    {
        if (Depth >= S::ScriptLimit)
        {
            return nullptr;
        }
        SavedCwdLength[Depth] = CwdLength;
        for (Size i{0U}; i < CwdLength; ++i)
        {
            SavedCwd[Depth][i] = CwdBytes[i];
        }
        ++Depth;
        Engines[Depth] = ::new (Storage + Depth * sizeof(S::Engine)) S::Engine{Filesystem(), Host()};
        Engines[Depth]->CopyStateFrom(parent);
        return Engines[Depth];
    }
    void Release() noexcept
    {
        --Depth;
        CwdLength = SavedCwdLength[Depth];
        for (Size i{0U}; i < CwdLength; ++i)
        {
            CwdBytes[i] = SavedCwd[Depth][i];
        }
    }
    static bool Launch(void* context, const S::Fields& args, const S::State& state, int& status) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        ++f.Launches;
        f.LaunchArguments = args;
        f.LaunchState = state;
        if (f.FailLaunch)
        {
            return false;
        }
        status = S::Equal(args.At(0U), S::Literal("missing")) ? 127 : 7;
        return true;
    }
    static bool Write(void* context, Foundation::UInt16 fd, S::Text text) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        return (fd == 2U ? f.Errors : *f.Sink).Append(text);
    }
    static bool Read(void* context, Foundation::UInt16, S::Buffer& out, bool& eof) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        eof = false;
        if (f.InputCursor == f.Input.Length)
        {
            eof = true;
            return true;
        }
        while (f.InputCursor < f.Input.Length)
        {
            const char c = f.Input.Data[f.InputCursor++];
            if (!out.Put(c))
            {
                return false;
            }
            if (c == '\n')
            {
                return true;
            }
        }
        eof = true;
        return true;
    }
    static bool Begin(void* context, const S::ExpandedRedirect* redirects, Size count,
                      Foundation::UInt64& cookie) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        if (f.FailRedirect)
        {
            return false;
        }
        ++f.Begins;
        cookie = f.Begins;
        f.RedirectTargets.Count = 0U;
        f.RedirectTargets.Used = 0U;
        for (Size i{0U}; i < count; ++i)
        {
            f.RedirectKinds[i] = redirects[i].Kind;
            f.RedirectFds[i] = redirects[i].Descriptor;
            if (!f.RedirectTargets.Add(redirects[i].Target))
            {
                return false;
            }
        }
        return true;
    }
    static void End(void* context, Foundation::UInt64) noexcept
    {
        ++static_cast<FakeHost*>(context)->Ends;
    }
    static bool Capture(void* context, S::Engine& parent, S::Text source, S::Buffer& out, int& status) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        ++f.Captures;
        auto* child = f.Child(parent);
        if (child == nullptr)
        {
            return false;
        }
        S::Buffer* previous = f.Sink;
        f.Sink = &out;
        const auto result = child->Run(source);
        status = result.Status;
        f.Sink = previous;
        f.Release();
        if (!result.Ok() && f.ChildFailure == S::Error::None)
        {
            f.ChildFailure = result.Code;
        }
        return result.Ok();
    }
    static bool Isolate(void* context, S::Isolation kind, S::Engine& parent, const S::Program& program,
                        Foundation::UInt16 node, int& status, Foundation::UInt64& pid) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        ++f.Isolations;
        if (kind == S::Isolation::Background)
        {
            pid = 42U;
        }
        if (kind == S::Isolation::Pipeline)
        {
            const Size level = f.Depth;
            if (level >= S::ScriptLimit)
            {
                return false;
            }
            S::Buffer* savedSink = f.Sink;
            const S::Text savedInput = f.Input;
            const Size savedCursor = f.InputCursor;
            S::Text input{};
            Size stage{0U};
            int failure{0};
            bool ok{true};
            for (auto n = program.Nodes[node].Left; n != 0U; n = program.Nodes[n].Next)
            {
                S::Buffer pipe{f.PipeBytes[level][stage++ % 2U], S::ExpansionLimit};
                f.Sink = program.Nodes[n].Next == 0U ? savedSink : &pipe;
                f.Input = input;
                f.InputCursor = 0U;
                auto* child = f.Child(parent);
                if (child == nullptr)
                {
                    ok = false;
                    break;
                }
                const auto result = child->Execute(program, n);
                f.Release();
                status = result.Status;
                if (status != 0)
                {
                    failure = status;
                }
                if (!result.Ok())
                {
                    ok = false;
                    break;
                }
                input = pipe.View();
            }
            f.Sink = savedSink;
            f.Input = savedInput;
            f.InputCursor = savedCursor;
            if (parent.Environment.Pipefail)
            {
                status = failure;
            }
            return ok;
        }
        auto* child = f.Child(parent);
        if (child == nullptr)
        {
            return false;
        }
        const auto result = child->Execute(program, node);
        f.Release();
        status = result.Status;
        return result.Ok();
    }
    static bool Resolve(void*, S::Text command, const S::State&, S::Buffer& out) noexcept
    {
        return !S::Equal(command, S::Literal("missing")) && out.Append(S::Literal("/apps/")) && out.Append(command);
    }
    static bool IsTerminal(void*, Foundation::UInt16 fd) noexcept
    {
        return fd < 3U;
    }
    static bool Home(void*, S::Text user, S::Buffer& out) noexcept
    {
        return S::Equal(user, S::Literal("alice")) && out.Append(S::Literal("/users/alice"));
    }
    static bool Cd(void* context, S::Text path) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        if (S::Equal(path, S::Literal("/missing")) || path.Length > sizeof(f.CwdBytes))
        {
            return false;
        }
        f.CwdLength = path.Length;
        for (Size i{0U}; i < path.Length; ++i)
        {
            f.CwdBytes[i] = path.Data[i];
        }
        return true;
    }
    static bool Pwd(void* context, S::Buffer& out) noexcept
    {
        auto& f = *static_cast<FakeHost*>(context);
        return out.Append({f.CwdBytes, f.CwdLength});
    }
    static bool ReadFile(void* context, S::Text, S::Buffer& out) noexcept
    {
        return out.Append(static_cast<FakeHost*>(context)->Script);
    }
    static bool List(void*, S::Text directory, S::Fields& out) noexcept
    {
        if (S::Equal(directory, S::Literal(".")) || S::Equal(directory, S::Literal("./.")) ||
            S::Equal(directory, S::Literal("..")) || S::Equal(directory, S::Literal("dir/..")) ||
            S::Equal(directory, S::Literal("link/..")) || S::Equal(directory, S::Literal("/work")))
        {
            return out.Add(S::Literal("z.cpp")) && out.Add(S::Literal("a.cpp")) && out.Add(S::Literal("b.txt")) &&
                   out.Add(S::Literal(".hidden")) && out.Add(S::Literal("dir")) && out.Add(S::Literal("link")) &&
                   out.Add(S::Literal("日本.txt"));
        }
        if (S::Equal(directory, S::Literal("dir")) || S::Equal(directory, S::Literal("./dir")) ||
            S::Equal(directory, S::Literal("dir/.")))
        {
            return out.Add(S::Literal("z.txt")) && out.Add(S::Literal("a.txt"));
        }
        return false;
    }
    static bool Inspect(void*, S::Text path, S::FileInfo& out) noexcept
    {
        const bool directory =
            S::Equal(path, S::Literal(".")) || S::Equal(path, S::Literal("..")) ||
            S::Equal(path, S::Literal("dir/..")) || S::Equal(path, S::Literal("link/..")) ||
            S::Equal(path, S::Literal("dir/.")) || S::Equal(path, S::Literal("./dir")) ||
            S::Equal(path, S::Literal("dir")) ||
            (path.Length != 0U && path.Data[0U] == '/' && !S::Equal(path, S::Literal("/work/a.cpp")));
        out = {true, directory, !directory, true, true, true, 12U};
        if (S::Equal(path, S::Literal("missing")) || S::Equal(path, S::Literal("/absent/tools")))
        {
            out.Exists = false;
        }
        out.Block = S::Equal(path, S::Literal("block"));
        out.Character = S::Equal(path, S::Literal("character"));
        out.Fifo = S::Equal(path, S::Literal("fifo"));
        out.Socket = S::Equal(path, S::Literal("socket"));
        out.SetUserId = S::Equal(path, S::Literal("setuid"));
        out.SetGroupId = S::Equal(path, S::Literal("setgid"));
        out.SymbolicLink = S::Equal(path, S::Literal("link")) || S::Equal(path, S::Literal("dangling"));
        if (S::Equal(path, S::Literal("dangling")))
        {
            out.Exists = false;
        }
        out.Device = 1U;
        out.Identity = S::Equal(path, S::Literal("alias")) ? 1U : 2U;
        if (S::Equal(path, S::Literal("first")))
        {
            out.Identity = 1U;
        }
        out.ModifiedSeconds = S::Equal(path, S::Literal("newer")) ? 2 : 1;
        out.ModifiedNanoseconds = S::Equal(path, S::Literal("nano")) ? 5U : 0U;
        return true;
    }
};
} // namespace Asteria::Test::ShellFixture
#endif
