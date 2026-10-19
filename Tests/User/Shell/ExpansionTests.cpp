// SPDX-License-Identifier: Apache-2.0
#include "User/Shell/Shell.hpp"
#include "User/Shell/FakeHost.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace S = Asteria::User::Shell;
static Asteria::Test::ShellFixture::FakeHost fixture{};
static S::Fields fields{};
struct Row
{
    const char* Rule;
    const char* Word;
    const char* Expected[5U];
    Asteria::Foundation::Size Count;
    const char* AssignedName{nullptr};
    const char* AssignedValue{nullptr};
};
// Expected fields are hand-derived from XCU §§2.2 and 2.6.1–2.6.7.
constexpr Row Rows[]{
    {"parameter continuation", "${v\\\n}", {"value"}, 1U},
    {"literal", "literal", {"literal"}, 1U},
    {"empty prefix splitting", "\"\"$x", {"", "a", "b"}, 3U},
    {"empty suffix splitting", "$x\"\"", {"a", "b", ""}, 3U},
    {"colon does not imply tilde", "word:~alice", {"word:~alice"}, 1U},
    {"substitution case delimiters", "$(case x in (x) echo good;; esac)", {"good"}, 1U},
    {"substitution comment", "$(echo good # ) ' ignored\n)", {"good"}, 1U},
    {"substitution here-doc", "$(echo good <<EOF\n) ' $(ignored)\nEOF\n)", {"good"}, 1U},
    {"single quote", "'$x'", {"$x"}, 1U},
    {"double quote", "\"$x\"", {" a  b "}, 1U},
    {"escape", "a\\ b", {"a b"}, 1U},
    {"double backslash", "\"a\\q\\\"b\"", {"a\\q\"b"}, 1U},
    {"empty quotes", "''", {""}, 1U},
    {"quote concatenate", "a''b\"c\"", {"abc"}, 1U},
    {"line continuation", "a\\\nb", {"ab"}, 1U},
    {"quoted continuation", "'a\\\nb'", {"a\\\nb"}, 1U},
    {"tilde", "~/file", {"/home/test/file"}, 1U},
    {"user tilde", "~alice/file", {"/users/alice/file"}, 1U},
    {"quoted tilde", "'~'", {"~"}, 1U},
    {"unknown tilde", "~nobody", {"~nobody"}, 1U},
    {"unquoted parameter", "$x", {"a", "b"}, 2U},
    {"provenance splitting", "p${x}s", {"p", "a", "b", "s"}, 4U},
    {"braces", "${x}", {"a", "b"}, 2U},
    {"unset", "$absent", {}, 0U},
    {"quoted unset", "\"$absent\"", {""}, 1U},
    {"null", "$empty", {}, 0U},
    {"default unset", "${absent-word}", {"word"}, 1U},
    {"default null with colon", "${empty:-word}", {"word"}, 1U},
    {"default null without colon", "${empty-word}", {}, 0U},
    {"default set", "${v:-word}", {"value"}, 1U},
    {"default empty word", "${absent-}", {}, 0U},
    {"default escaped space", "${absent:-a\\ b}", {"a b"}, 1U},
    {"default quoted empty", "${absent:-''}", {""}, 1U},
    {"assign default", "${new:=assigned}", {"assigned"}, 1U, "new", "assigned"},
    {"assign unquoted space", "${new:=a b}", {"a", "b"}, 2U, "new", "a b"},
    {"assign escaped space", "${new:=a\\ b}", {"a", "b"}, 2U, "new", "a b"},
    {"assign single quoted space", "${new:='a b'}", {"a", "b"}, 2U, "new", "a b"},
    {"assign double quoted space", "${new:=\"a b\"}", {"a", "b"}, 2U, "new", "a b"},
    {"assign without colon", "${new='a b'}", {"a", "b"}, 2U, "new", "a b"},
    {"assign null with colon", "${empty:='a b'}", {"a", "b"}, 2U, "empty", "a b"},
    {"assign null without colon", "${empty='a b'}", {}, 0U, "empty", ""},
    {"assign single quoted empty", "${new:=''}", {}, 0U, "new", ""},
    {"assign double quoted empty", "${new:=\"\"}", {}, 0U, "new", ""},
    {"assign outer quoted space", "\"${new:=a b}\"", {"a b"}, 1U, "new", "a b"},
    {"assign outer quoted empty", "\"${new:=}\"", {""}, 1U, "new", ""},
    {"assign empty quoted prefix", "''${new:=''}", {""}, 1U, "new", ""},
    {"assign quoted wildcard", "${new:='*.cpp'}", {"a.cpp", "z.cpp"}, 2U, "new", "*.cpp"},
    {"alternate unset", "${absent+word}", {}, 0U},
    {"alternate null", "${empty+word}", {"word"}, 1U},
    {"alternate null colon", "${empty:+word}", {}, 0U},
    {"alternate set", "${v:+word}", {"word"}, 1U},
    {"alternate escaped space", "${v:+a\\ b}", {"a b"}, 1U},
    {"alternate quoted empty", "${v:+''}", {""}, 1U},
    {"length UTF-8", "${#unicode}", {"3"}, 1U},
    {"length null", "${#empty}", {"0"}, 1U},
    {"short prefix", "${path#*/}", {"b/c.txt"}, 1U},
    {"long prefix", "${path##*/}", {"c.txt"}, 1U},
    {"short suffix", "${path%/*}", {"a/b"}, 1U},
    {"long suffix", "${path%%/*}", {"a"}, 1U},
    {"trim no match", "${path#Z*}", {"a/b/c.txt"}, 1U},
    {"quoted trim pattern", "${path#'*/'}", {"a/b/c.txt"}, 1U},
    {"outer quotes allow pattern", "\"${path##*/}\"", {"c.txt"}, 1U},
    {"nested default", "${absent:-${v}}", {"value"}, 1U},
    {"lazy substitution", "${v:-$(echo BAD)}", {"value"}, 1U},
    {"dollar substitution", "$(printf 'a b\n\n')", {"a", "b"}, 2U},
    {"quoted substitution", "\"$(printf 'a b\n\n')\"", {"a b"}, 1U},
    {"backquotes", "`echo hi`", {"hi"}, 1U},
    {"arithmetic precedence", "$((1+2*3))", {"7"}, 1U},
    {"arithmetic nested", "$(((2+3)*4))", {"20"}, 1U},
    {"arithmetic base", "$((010+0x10))", {"24"}, 1U},
    {"arithmetic unary", "$((-2 + ~0 + !0))", {"-2"}, 1U},
    {"arithmetic shift", "$((2<<3|1))", {"17"}, 1U},
    {"arithmetic comparison", "$((2<3 && 4>=4))", {"1"}, 1U},
    {"arithmetic short circuit", "$((0 && 1/0))", {"0"}, 1U},
    {"arithmetic conditional", "$((1?7:1/0))", {"7"}, 1U},
    {"arithmetic assignment", "$((n=4,n+=3,n))", {"7"}, 1U},
    {"arithmetic variable", "$((num+2))", {"7"}, 1U},
    {"arithmetic parameter", "$(($num+2))", {"7"}, 1U},
    {"glob character class", "[[:lower:]].cpp", {"a.cpp", "z.cpp"}, 2U},
    {"glob current directory", "./*.cpp", {"./a.cpp", "./z.cpp"}, 2U},
    {"glob parent", "../*.cpp", {"../a.cpp", "../z.cpp"}, 2U},
    {"glob parent after directory", "dir/../*.cpp", {"dir/../a.cpp", "dir/../z.cpp"}, 2U},
    {"glob parent preserves symlink traversal", "link/../*.cpp", {"link/../a.cpp", "link/../z.cpp"}, 2U},
    {"glob dot after directory", "dir/./*.txt", {"dir/./a.txt", "dir/./z.txt"}, 2U},
    {"glob dot after wildcard", "d*/./*.txt", {"dir/./a.txt", "dir/./z.txt"}, 2U},
    {"glob quoted dot", "'.'/*.cpp", {"./a.cpp", "./z.cpp"}, 2U},
    {"glob sorted", "*.cpp", {"a.cpp", "z.cpp"}, 2U},
    {"glob question scalar", "??.txt", {"日本.txt"}, 1U},
    {"glob bracket", "[ab].*", {"a.cpp", "b.txt"}, 2U},
    {"glob negation", "[!a].cpp", {"z.cpp"}, 1U},
    {"glob range", "[a-b].*", {"a.cpp", "b.txt"}, 2U},
    {"glob quoted", "'*.cpp'", {"*.cpp"}, 1U},
    {"glob no matches", "*.missing", {"*.missing"}, 1U},
    {"glob dot explicit", ".*", {".hidden"}, 1U},
    {"glob components", "d*/*.txt", {"dir/a.txt", "dir/z.txt"}, 2U},
    {"glob trailing slash", "d*/", {"dir/"}, 1U},
    {"quoted at", "\"$@\"", {"one two", "", "three"}, 3U},
    {"at concatenation", "pre\"$@\"post", {"preone two", "", "threepost"}, 3U},
    {"quoted star", "\"$*\"", {"one two  three"}, 1U},
    {"count", "$#", {"3"}, 1U},
    {"count length", "${##}", {"1"}, 1U},
    {"count default disambiguation", "${#-fallback}", {"3"}, 1U},
    {"position", "$1", {"one", "two"}, 2U},
    {"position without braces", "$10", {"one", "two0"}, 2U},
    {"position with braces", "${10}", {}, 0U},
    {"status", "$?", {"9"}, 1U},
    {"pid", "$$", {"123"}, 1U},
    {"background", "$!", {"42"}, 1U},
    {"name", "$0", {"sh"}, 1U},
    {"dollar single quotes", "$'a\\nb'", {"a\nb"}, 1U}};
} // Namespace
int main()
{
    Asteria::TestSupport::TestContext test{"Shell expansions (§2.6)"};
    for (const auto& row : Rows)
    {
        auto& engine = fixture.Reset();
        ASTERIA_CHECK(test, engine.Set(S::Literal("x"), S::Literal(" a  b ")).Ok());
        ASTERIA_CHECK(test, engine.Set(S::Literal("v"), S::Literal("value")).Ok());
        ASTERIA_CHECK(test, engine.Set(S::Literal("empty"), S::Literal("")).Ok());
        ASTERIA_CHECK(test, engine.Set(S::Literal("unicode"), S::Literal("日本語")).Ok());
        ASTERIA_CHECK(test, engine.Set(S::Literal("path"), S::Literal("a/b/c.txt")).Ok());
        ASTERIA_CHECK(test, engine.Set(S::Literal("num"), S::Literal("5")).Ok());
        ASTERIA_CHECK(test, engine.Run(S::Literal("set -- 'one two' '' three")).Ok());
        engine.Environment.LastStatus = 9;
        engine.Environment.ProcessId = 123U;
        engine.Environment.BackgroundId = 42U;
        fields.Count = 0U;
        fields.Used = 0U;
        const auto result = engine.Expand(S::Literal(row.Word), fields);
        ASTERIA_EXPECT(test, row.Rule, result.Ok(), fields.Count == row.Count);
        if (result.Ok() && fields.Count == row.Count)
        {
            for (Asteria::Foundation::Size i{0U}; i < fields.Count; ++i)
            {
                ASTERIA_EXPECT(test, row.Rule, S::Equal(fields.At(i), S::Literal(row.Expected[i])));
            }
        }
        if (row.AssignedName != nullptr)
        {
            bool present{false};
            const auto assigned = engine.Get(S::Literal(row.AssignedName), present);
            ASTERIA_EXPECT(test, row.Rule, present, S::Equal(assigned, S::Literal(row.AssignedValue)));
        }
        if (S::Equal(S::Literal(row.Rule), S::Literal("lazy substitution")))
        {
            ASTERIA_CHECK_EQ(test, fixture.Captures, 0U);
        }
    }
    auto& engine = fixture.Reset();
    struct ErrorRow
    {
        const char* Word;
        S::Error Error;
    };
    constexpr ErrorRow errors[]{{"${bad:?needed}", S::Error::ParameterUnset},
                                {"${1:=bad}", S::Error::InvalidName},
                                {"${bad:invalid}", S::Error::Syntax},
                                {"$((1/0))", S::Error::Arithmetic},
                                {"$((1<<64))", S::Error::Arithmetic},
                                {"$((08))", S::Error::Arithmetic},
                                {"$((1+))", S::Error::Arithmetic},
                                {"$((1=2))", S::Error::Arithmetic},
                                {"$'\\000'", S::Error::NulByte},
                                {"${unfinished", S::Error::UnterminatedExpansion}};
    for (const auto& row : errors)
    {
        fields.Count = 0U;
        fields.Used = 0U;
        ASTERIA_EXPECT(test, row.Word, engine.Expand(S::Literal(row.Word), fields).Code == row.Error);
    }
    ASTERIA_CHECK(test, engine.Run(S::Literal("set -u")).Ok());
    fields.Count = 0U;
    fields.Used = 0U;
    ASTERIA_CHECK_EQ(test, engine.Expand(S::Literal("$missing"), fields).Code, S::Error::ParameterUnset);
    ASTERIA_CHECK(test, engine.Set(S::Literal("IFS"), S::Literal(":")).Ok());
    ASTERIA_CHECK(test, engine.Set(S::Literal("v"), S::Literal(":a::b:")).Ok());
    fields.Count = 0U;
    fields.Used = 0U;
    ASTERIA_CHECK(test, engine.Expand(S::Literal("$v"), fields).Ok());
    ASTERIA_CHECK_EQ(test, fields.Count, 4U);
    ASTERIA_CHECK(test, S::Equal(fields.At(0U), S::Literal("")) && S::Equal(fields.At(1U), S::Literal("a")) &&
                            S::Equal(fields.At(2U), S::Literal("")) && S::Equal(fields.At(3U), S::Literal("b")));
    ASTERIA_CHECK(test, engine.Set(S::Literal("IFS"), S::Literal("")).Ok());
    fields.Count = 0U;
    fields.Used = 0U;
    ASTERIA_CHECK(test, engine.Expand(S::Literal("$v"), fields).Ok());
    ASTERIA_CHECK_EQ(test, fields.Count, 1U);
    ASTERIA_CHECK(test, engine.Run(S::Literal("set -- 'one two' '' three")).Ok());
    fields.Count = 0U;
    fields.Used = 0U;
    ASTERIA_CHECK(test, engine.Expand(S::Literal("$*"), fields).Ok());
    ASTERIA_CHECK_EQ(test, fields.Count, 2U);
    ASTERIA_CHECK(test, S::Equal(fields.At(0U), S::Literal("one two")) && S::Equal(fields.At(1U), S::Literal("three")));
    ASTERIA_CHECK(test, engine.Run(S::Literal("set --")).Ok());
    fields.Count = 0U;
    fields.Used = 0U;
    ASTERIA_CHECK(test, engine.Expand(S::Literal("\"$@\""), fields).Ok());
    ASTERIA_CHECK_EQ(test, fields.Count, 0U);
    static char large[S::ValueLimit + 1U]{};
    for (auto& c : large)
    {
        c = 'x';
    }
    ASTERIA_CHECK_EQ(test, engine.Set(S::Literal("large"), {large, sizeof(large)}).Code, S::Error::ValueCapacity);
    ASTERIA_CHECK(test, engine.Set(S::Literal("large"), {large, S::ValueLimit}).Ok());
    fields.Count = 0U;
    fields.Used = 0U;
    ASTERIA_CHECK_EQ(test,
                     engine.Expand(S::Literal("$large$large$large$large$large$large$large$large$large"), fields).Code,
                     S::Error::ExpansionCapacity);
    fields.Count = S::FieldLimit;
    fields.Used = 0U;
    ASTERIA_CHECK_EQ(test, engine.Expand(S::Literal("value"), fields).Code, S::Error::FieldCapacity);
    ASTERIA_CHECK_EQ(test, engine.Set(S::Literal("1bad"), S::Literal("value")).Code, S::Error::InvalidName);
    auto& full = fixture.Reset();
    for (Asteria::Foundation::Size i{0U}; i < S::VariableLimit - 2U; ++i)
    {
        char name[32U]{};
        const int length = std::snprintf(name, sizeof(name), "v%zu", i);
        ASTERIA_CHECK(test,
                      length > 0 &&
                          full.Set({name, static_cast<Asteria::Foundation::Size>(length)}, S::Literal("value")).Ok());
    }
    ASTERIA_CHECK_EQ(test, full.Set(S::Literal("extra"), S::Literal("value")).Code, S::Error::VariableCapacity);
    return test.Finish();
}
