// SPDX-License-Identifier: Apache-2.0
#include "User/Shell/Shell.hpp"
#include "User/Shell/FakeHost.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace S = Asteria::User::Shell;
static Asteria::Test::ShellFixture::FakeHost fixture{};
struct Row
{
    const char* Rule;
    const char* Script;
    const char* Output;
    int Status;
};
constexpr Row Rows[]{
    {"set reusable option listing", "set -uf; saved=$(set +o); set +uf; eval \"$saved\"; echo $-", "fu\n", 0},
    {"set quoted variable listing", "v=\"a'b\"; saved=$(set); v=changed; eval \"$saved\"; echo \"$v\"", "a'b\n", 0},
    {"cd CDPATH", "CDPATH=/absent:/search; cd tools; pwd", "/search/tools\n/search/tools\n", 0},
    {"cd CDPATH empty", "CDPATH=:/search; cd dir; pwd", "/work/dir\n", 0},
    {"cd CDPATH dot bypass", "CDPATH=/search; cd ./dir; pwd", "/work/dir\n", 0},
    {"cd rejects file before dot-dot", "cd a.cpp/..", "", 1},
    {"cd empty", "cd ''", "", 1},
    {"test two literal operators", "test ! '!'; echo $?; test ! '('; echo $?; test -n '!'; echo $?", "1\n1\n0\n", 0},
    {"test binary before negation", "test '!' = '!'", "", 0},
    {"test four negation", "test ! ! -n x", "", 0},
    {"test terminal", "test -t 0; echo $?; test -t 4; echo $?", "0\n1\n", 0},
    {"test file kinds", "test -b block && test -c character && test -p fifo && test -S socket", "", 0},
    {"test file modes", "test -u setuid && test -g setgid", "", 0},
    {"test symlink", "test -h link && test -L dangling && ! test -e dangling", "", 0},
    {"test file identity", "test first -ef alias; echo $?; test first -ef newer; echo $?", "0\n1\n", 0},
    {"test file timestamps", "test newer -nt first && test first -ot newer && test nano -nt first", "", 0},
    {"test missing timestamps", "test newer -nt missing && test missing -ot first && ! test missing -nt missing", "",
     0},
    {"case fallthrough", "case x in x) echo first;& y) echo second;; esac", "first\nsecond\n", 0},
    {"declaration assignment", "v='one two'; export x=$v y=~/file; echo \"$x\" \"$y\"", "one two /home/test/file\n", 0},
    {"assignment tilde colon", "v=~alice:~/file; echo $v", "/users/alice:/home/test/file\n", 0},
    {"numbered printf", "printf '%2$s/%1$s\n' one two three four", "two/one\nfour/three\n", 0},
    {"export unassigned", "export new; echo ${new+set}; new=value; echo ${new+set}", "\nset\n", 0},
    {"here-doc line continuation", "external <<EOF\ntext\\\nEOF\nEOF\n", "", 7},
    {"reserved word continuation", "i\\\nf true; th\\\nen echo good; fi", "good\n", 0},
    {"name continuation", "na\\\nme=value; echo $name", "value\n", 0},
    {"errexit", "set -e; false; echo BAD", "", 1},
    {"errexit conditional", "set -e; false && echo BAD; if false; then :; fi; echo good", "good\n", 0},
    {"errexit final AND", "set -e; true && false; echo BAD", "", 1},
    {"pipefail", "set -o pipefail; false | true; echo $?", "1\n", 0},
    {"noexec", "set -n; echo BAD", "", 0},
    {"function delayed here-doc", "f() { echo hello; } <<'END'\ndocument\nEND\nf", "hello\n", 0},
    {"expanded pattern escape", "p='a\\*'; case 'a*' in $p) echo good;; esac", "good\n", 0},
    {"assignment positional join", "set -- 'one two' '' three; v=\"$@\"; echo \"$v\"", "one two  three\n", 0},
    {"subshell return scope", "f() { (return 3); echo $?; }; f", "3\n", 0},
    {"status true", "true", "", 0},
    {"status false", "false", "", 1},
    {"colon", ": arg", "", 0},
    {"sequence", "echo one; echo two", "one\ntwo\n", 0},
    {"and short circuit", "false && echo BAD; echo $?", "1\n", 0},
    {"or short circuit", "true || echo BAD; echo $?", "0\n", 0},
    {"and/or associativity", "true || echo BAD && echo good", "good\n", 0},
    {"negate", "! true", "", 1},
    {"if else", "if false; then echo BAD; else echo good; fi", "good\n", 0},
    {"if elif", "if false; then :; elif true; then echo good; else echo BAD; fi", "good\n", 0},
    {"if no selected branch", "if false; then false; fi", "", 0},
    {"while", "i=0; while test $i -lt 3; do echo $i; i=$((i+1)); done", "0\n1\n2\n", 0},
    {"until", "i=0; until [ $i -eq 2 ]; do echo $i; i=$((i+1)); done", "0\n1\n", 0},
    {"for", "for x in 'one two' three; do echo \"$x\"; done", "one two\nthree\n", 0},
    {"for implicit", "set -- 'one two' three; for x; do echo \"$x\"; done", "one two\nthree\n", 0},
    {"for empty", "for x in; do echo BAD; done", "", 0},
    {"case", "case alpha in b*) echo BAD;; a*|c*) echo good;; *) echo BAD;; esac", "good\n", 0},
    {"case quoted pattern", "case '*' in '*') echo good;; esac", "good\n", 0},
    {"case no match", "case x in y) false;; esac", "", 0},
    {"brace environment", "v=old; { v=new; }; echo $v", "new\n", 0},
    {"subshell environment", "v=old; (v=new; echo $v); echo $v", "new\nold\n", 0},
    {"pipeline", "echo one | false; echo $?", "1\n", 0},
    {"pipeline transport", "printf 'one two\n' | { read a b; echo \"$b/$a\"; }; echo ${a-unset}", "two/one\nunset\n",
     0},
    {"subshell directory isolation", "(cd /new; pwd -P); pwd -P", "/new\n/work\n", 0},
    {"pipeline environment", "v=old; v=new | true; echo $v", "old\n", 0},
    {"background", "echo job & echo $!", "job\n42\n", 0},
    {"functions", "f() { echo \"$1\"; return 3; echo BAD; }; set -- outer; f inner; echo $? $1", "inner\n3 outer\n", 0},
    {"function persists across eval", "eval 'f() { echo hello; }'; f", "hello\n", 0},
    {"function temporary parameters", "f() { set -- changed; }; set -- original; f arg; echo $1", "original\n", 0},
    {"regular builtin assignment", "v=old; v=new echo $v; echo $v", "old\nold\n", 0},
    {"special builtin assignment", "v=old; v=new :; echo $v", "new\n", 0},
    {"assignment rhs no splitting", "v='one two'; x=$v; echo \"$x\"", "one two\n", 0},
    {"assignment-only status", "false; x=value; echo $?", "0\n", 0},
    {"assignment substitution status", "x=$(false); echo $?", "1\n", 0},
    {"substitution isolation", "v=old; echo \"$(v=new; echo $v)\"; echo $v", "new\nold\n", 0},
    {"nested substitutions", "echo \"$(echo \"$(echo nested)\")\"", "nested\n", 0},
    {"positional shift", "set -- a b c; shift 2; echo $# $1", "1 c\n", 0},
    {"shift error", "set -- a; shift 2", "", 2},
    {"unset", "v=value; unset v; echo \"${v-unset}\"", "unset\n", 0},
    {"eval", "v=old; eval 'v=new; echo $v'; echo $v", "new\nnew\n", 0},
    {"source", ". /script argument; echo $? $source_value", "4 argument\n", 0},
    {"exit", "echo before; exit 7; echo BAD", "before\n", 7},
    {"return outside function", "return 4", "", 2},
    {"pwd", "pwd", "/work\n", 0},
    {"cd", "cd /new; pwd", "/new\n", 0},
    {"cd relative", "cd one/../two; pwd; cd -; pwd", "/work/two\n/work\n/work\n", 0},
    {"cd home", "cd; pwd", "/home/test\n", 0},
    {"cd error", "cd /missing", "", 1},
    {"printf format reuse", "printf '%s:%d\n' one 1 two 2", "one:1\ntwo:2\n", 0},
    {"printf escapes", "printf '%b' 'a\\nb\\cBAD'; echo good", "a\nbgood\n", 0},
    {"printf width", "printf '[%5s][%-3s][%04d][%.2s]' a b 7 xyz", "[    a][b  ][0007][xy]", 0},
    {"printf alternate", "printf '%#x %#o %c %%' 16 8 yes", "0x10 010 y %", 0},
    {"printf default operands", "printf '%s:%d'", ":0", 0},
    {"test empty", "test", "", 1},
    {"test literal !", "test '!'", "", 0},
    {"test string", "test foo = foo", "", 0},
    {"test negate", "test ! foo", "", 1},
    {"test integer", "test -3 -lt 4", "", 0},
    {"test unary", "test -z ''", "", 0},
    {"test file", "test -f a.cpp", "", 0},
    {"test directory", "test -d dir", "", 0},
    {"test nonexistent", "test -e missing", "", 1},
    {"bracket close", "[ x = x ]", "", 0},
    {"bracket missing close", "[ x", "", 2},
    {"noglob", "set -f; echo *.cpp", "*.cpp\n", 0},
    {"echo conservative choice", "echo -n 'a\\b'", "-n a\\b\n", 0},
    {"type builtin and function", "f() { :; }; type echo f", "echo is a shell builtin\nf is a shell function\n", 0},
    {"type external", "type external", "external is /apps/external\n", 0},
    {"external status", "external arg", "", 7},
    {"lazy here-doc", "false && external <<EOF\n$(echo BAD)\nEOF\necho good", "good\n", 0},
    {"UTF-8", "v=日本語; echo \"$v\" ${#v}", "日本語 3\n", 0}};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext test{"Shell execution (§2.9 and utilities)"};
    for (const auto& row : Rows)
    {
        auto& engine = fixture.Reset();
        const auto result = engine.Run(S::Literal(row.Script));
        ASTERIA_EXPECT(test, row.Rule, result.Ok(), result.Status == row.Status,
                       S::Equal(fixture.Output.View(), S::Literal(row.Output)));
        if (!result.Ok())
        {
            static_cast<void>(
                std::fprintf(stderr, "  error=%u offset=%zu\n", static_cast<unsigned>(result.Code), result.Offset));
        }
        else if (!S::Equal(fixture.Output.View(), S::Literal(row.Output)))
        {
            static_cast<void>(std::fprintf(stderr, "  actual=%.*s expected=%s\n",
                                           static_cast<int>(fixture.Output.Length), fixture.Output.Data, row.Output));
        }
    }
    auto& engine = fixture.Reset();
    ASTERIA_CHECK(test, engine.Run(S::Literal("export A=value; B=temporary external '$A'")).Ok());
    ASTERIA_CHECK_EQ(test, fixture.Launches, 1U);
    ASTERIA_CHECK_EQ(test, fixture.LaunchArguments.Count, 2U);
    ASTERIA_CHECK(test, S::Equal(fixture.LaunchArguments.At(1U), S::Literal("$A")));
    bool seenA{false};
    bool seenB{false};
    for (const auto& v : fixture.LaunchState.Variables)
    {
        if (v.Present && v.Exported && S::Equal({v.Name, v.NameLength}, S::Literal("A")))
        {
            seenA = true;
        }
        if (v.Present && v.Exported && S::Equal({v.Name, v.NameLength}, S::Literal("B")))
        {
            seenB = true;
        }
    }
    ASTERIA_CHECK(test, seenA && seenB);
    bool present{false};
    static_cast<void>(engine.Get(S::Literal("B"), present));
    ASTERIA_CHECK(test, !present);
    ASTERIA_CHECK(test, engine.Run(S::Literal("echo hi >out 2>&1")).Ok());
    ASTERIA_CHECK_EQ(test, fixture.Begins, 1U);
    ASTERIA_CHECK_EQ(test, fixture.Ends, 1U);
    ASTERIA_CHECK_EQ(test, fixture.RedirectKinds[0U], S::TokenKind::Great);
    ASTERIA_CHECK_EQ(test, fixture.RedirectFds[0U], 1U);
    ASTERIA_CHECK_EQ(test, fixture.RedirectKinds[1U], S::TokenKind::GreatAnd);
    ASTERIA_CHECK_EQ(test, fixture.RedirectFds[1U], 2U);
    ASTERIA_CHECK(test, S::Equal(fixture.RedirectTargets.At(0U), S::Literal("out")) &&
                            S::Equal(fixture.RedirectTargets.At(1U), S::Literal("1")));
    ASTERIA_CHECK(test, engine.Run(S::Literal("x=value; external <<EOF <<-'Q'\n\"$x\"\nEOF\n\t$x\n\tQ\n")).Ok());
    ASTERIA_CHECK(test, S::Equal(fixture.RedirectTargets.At(0U), S::Literal("\"value\"\n")) &&
                            S::Equal(fixture.RedirectTargets.At(1U), S::Literal("$x\n")));
    ASTERIA_CHECK(test, engine.Run(S::Literal("external <<-E\nx\\\n\ty\nE\n")).Ok());
    ASTERIA_CHECK(test, S::Equal(fixture.RedirectTargets.At(0U), S::Literal("x\ty\n")));
    ASTERIA_CHECK(test, engine.Run(S::Literal("external <<-E\nx\\\\\n\ty\nE\n")).Ok());
    ASTERIA_CHECK(test, S::Equal(fixture.RedirectTargets.At(0U), S::Literal("x\\\ny\n")));
    fixture.Input = S::Literal("  one  two three  \n");
    fixture.InputCursor = 0U;
    ASTERIA_CHECK(test, engine.Run(S::Literal("read a b")).Ok());
    ASTERIA_CHECK(test, S::Equal(engine.Get(S::Literal("a"), present), S::Literal("one")) && present);
    ASTERIA_CHECK(test, S::Equal(engine.Get(S::Literal("b"), present), S::Literal("two three")) && present);
    fixture.Input = S::Literal("a\\ b c\n");
    fixture.InputCursor = 0U;
    ASTERIA_CHECK(test, engine.Run(S::Literal("read a b")).Ok());
    ASTERIA_CHECK(test, S::Equal(engine.Get(S::Literal("a"), present), S::Literal("a b")));
    fixture.Input = S::Literal("a\\\nb\n");
    fixture.InputCursor = 0U;
    ASTERIA_CHECK(test, engine.Run(S::Literal("read a")).Ok());
    ASTERIA_CHECK(test, S::Equal(engine.Get(S::Literal("a"), present), S::Literal("ab")));
    fixture.FailLaunch = true;
    const auto failed = engine.Run(S::Literal("A=temporary external >out"));
    ASTERIA_CHECK_EQ(test, failed.Code, S::Error::Host);
    ASTERIA_CHECK_EQ(test, fixture.Begins, fixture.Ends);
    ASTERIA_CHECK(test, S::Equal(engine.Get(S::Literal("A"), present), S::Literal("value")));
    fixture.FailLaunch = false;
    ASTERIA_CHECK_EQ(test, engine.Run(S::Literal("while true; do :; done")).Code, S::Error::Steps);
    ASTERIA_CHECK_EQ(test, engine.Run(S::Literal("f() { f; }; f")).Code, S::Error::Depth);
    auto& recursive = fixture.Reset();
    ASTERIA_CHECK_EQ(test, recursive.Run(S::Literal("v='$(eval \"$v\")'; eval \"$v\"")).Code, S::Error::Host);
    ASTERIA_CHECK_EQ(test, fixture.ChildFailure, S::Error::Depth);
    ASTERIA_CHECK(test, fixture.Captures < S::ScriptLimit);
    fixture.Reset().Environment.NoClobber = true;
    ASTERIA_CHECK(test, fixture.Engines[0U]->Run(S::Literal("echo hi >out")).Ok());
    ASTERIA_CHECK(test, fixture.Engines[0U]->Run(S::Literal("external <<$'END'\n$x\nEND\n")).Ok());
    ASTERIA_CHECK(test, S::Equal(fixture.RedirectTargets.At(0U), S::Literal("$x\n")));
    static S::Program program{};
    ASTERIA_CHECK(test, S::Parse(S::Literal("echo value"), program).Ok());
    program.Nodes[program.Root].Left = static_cast<Asteria::Foundation::UInt16>(S::NodeLimit + 1U);
    ASTERIA_CHECK_EQ(test, fixture.Engines[0U]->Execute(program).Code, S::Error::InvalidProgram);
    fixture.Reset().Environment.ParameterCount = S::FieldLimit + 1U;
    ASTERIA_CHECK_EQ(test, fixture.Engines[0U]->Run(S::Literal("echo $@")).Code, S::Error::FieldCapacity);
    fixture.Reset().Environment.Name.Length = static_cast<Asteria::Foundation::UInt16>(S::ValueLimit + 1U);
    ASTERIA_CHECK_EQ(test, fixture.Engines[0U]->Run(S::Literal("echo $0")).Code, S::Error::ValueCapacity);
    return test.Finish();
}
