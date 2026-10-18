// SPDX-License-Identifier: Apache-2.0
#include "User/Shell/Shell.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace S = Asteria::User::Shell;
using Asteria::Foundation::Size;
struct Row
{
    const char* Rule;
    const char* Source;
    S::NodeKind Kind;
};
// IEEE Std 1003.1-2024 XCU §2.10.2: independent examples for every
// required production family; expected AST kind is below the outer list.
constexpr Row Grammar[]{{"simple command", "echo hello", S::NodeKind::Simple},
                        {"assignment prefix", "A=one B=two cmd arg", S::NodeKind::Simple},
                        {"assignment only", "A=one", S::NodeKind::Simple},
                        {"redirect prefix", ">out cmd", S::NodeKind::Simple},
                        {"redirect suffix", "cmd <in >out", S::NodeKind::Simple},
                        {"redirect only", "2>out", S::NodeKind::Simple},
                        {"pipeline", "echo x | external | false", S::NodeKind::Pipeline},
                        {"negated pipeline", "! true | false", S::NodeKind::Negate},
                        {"AND", "true && echo ok", S::NodeKind::And},
                        {"OR", "false || echo ok", S::NodeKind::Or},
                        {"AND/OR equal precedence", "a || b && c", S::NodeKind::And},
                        {"sequential separator", "one; two", S::NodeKind::Simple},
                        {"background separator", "one & two", S::NodeKind::Background},
                        {"newline lists", "\none\ntwo\n", S::NodeKind::Simple},
                        {"subshell", "(one; two)", S::NodeKind::Subshell},
                        {"brace group", "{ one; two; }", S::NodeKind::Group},
                        {"compound redirects", "{ one; } <in 2>&1", S::NodeKind::Group},
                        {"if", "if true; then one; fi", S::NodeKind::If},
                        {"if else", "if false; then one; else two; fi", S::NodeKind::If},
                        {"if elif", "if a; then b; elif c; then d; elif e; then f; else g; fi", S::NodeKind::If},
                        {"while", "while true; do one; done", S::NodeKind::While},
                        {"until", "until false; do one; done", S::NodeKind::Until},
                        {"for implicit", "for item; do echo $item; done", S::NodeKind::For},
                        {"for implicit without separator", "for item do echo $item; done", S::NodeKind::For},
                        {"for explicit", "for item in one two; do echo $item; done", S::NodeKind::For},
                        {"for empty", "for item in; do :; done", S::NodeKind::For},
                        {"for newline", "for item\nin one two\ndo :\ndone", S::NodeKind::For},
                        {"case empty", "case x in esac", S::NodeKind::Case},
                        {"case patterns", "case x in (one|two) echo yes;; *) echo no;; esac", S::NodeKind::Case},
                        {"case empty body and optional terminator", "case x in x) ;; y) esac", S::NodeKind::Case},
                        {"function", "f() { echo hello; }", S::NodeKind::Function},
                        {"function subshell", "f()\n(echo hello) >out", S::NodeKind::Function},
                        {"linebreak after operator", "true &&\necho x |\ncat", S::NodeKind::And},
                        {"quoting", "echo 'if' \"a | b\" c\\ d", S::NodeKind::Simple},
                        {"continuation", "echo ab\\\ncd", S::NodeKind::Simple},
                        {"nested expansions", "echo \"${x:-$(printf '%s' \"$((1+2))\")}\"", S::NodeKind::Simple},
                        {"here-doc", "cat <<EOF\nhello\nEOF\n", S::NodeKind::Simple},
                        {"here-doc queue", "cat <<'A' <<-B\n$x\nA\n\t$x\n\tB\n", S::NodeKind::Simple},
                        {"reserved words as operands", "echo if then done esac ! { }", S::NodeKind::Simple},
                        {"UTF-8 bytes", "echo '日本語' >日本.txt", S::NodeKind::Simple}};
struct Bad
{
    const char* Source;
    S::Error Code;
};
constexpr Bad Errors[]{{"echo '", S::Error::UnterminatedQuote},
                       {"echo \"", S::Error::UnterminatedQuote},
                       {"echo \\", S::Error::UnterminatedQuote},
                       {"echo ${x", S::Error::UnterminatedExpansion},
                       {"echo $(echo x", S::Error::UnterminatedExpansion},
                       {"echo `echo x", S::Error::UnterminatedQuote},
                       {"| echo x", S::Error::Syntax},
                       {"a &&", S::Error::Syntax},
                       {"a ||", S::Error::Syntax},
                       {"a |", S::Error::Syntax},
                       {";;", S::Error::Syntax},
                       {"a; ; b", S::Error::Syntax},
                       {"a )", S::Error::Syntax},
                       {"(echo x", S::Error::Syntax},
                       {"()", S::Error::Syntax},
                       {"{ echo x;", S::Error::Syntax},
                       {"{ echo x }", S::Error::Syntax},
                       {"if true; then echo x;", S::Error::Syntax},
                       {"if true; fi", S::Error::Syntax},
                       {"while true; done", S::Error::Syntax},
                       {"for 1 in a; do :; done", S::Error::InvalidName},
                       {"case x in a echo bad;; esac", S::Error::Syntax},
                       {"f() echo x", S::Error::Syntax},
                       {"echo >", S::Error::Syntax},
                       {"cat <<EOF\nmissing\n", S::Error::MissingHereDocument},
                       {"cat <<EOF", S::Error::MissingHereDocument},
                       {"cat << ;", S::Error::Syntax}};
static S::Program program{};
static char capacity[S::InputLimit + 2U]{};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext test{"Shell grammar (§2.10.2)"};
    for (const auto& row : Grammar)
    {
        const auto result = S::Parse(S::Literal(row.Source), program);
        ASTERIA_EXPECT(test, row.Rule, result.Ok(), program.Root != 0U,
                       program.Nodes[program.Root].Kind == S::NodeKind::List,
                       program.Nodes[program.Nodes[program.Root].Left].Kind == row.Kind);
    }
    for (const auto& row : Errors)
    {
        const auto result = S::Parse(S::Literal(row.Source), program);
        ASTERIA_EXPECT(test, row.Source, result.Code == row.Code, result.Offset <= S::Literal(row.Source).Length);
    }
    ASTERIA_CHECK(test, S::Parse(S::Literal(" # only comment\n\n"), program).Ok());
    ASTERIA_CHECK_EQ(test, program.Root, 0U);
    ASTERIA_CHECK(test, S::Parse(S::Literal("a || b && c"), program).Ok());
    const auto root = program.Nodes[program.Root].Left;
    ASTERIA_CHECK_EQ(test, program.Nodes[program.Nodes[root].Left].Kind, S::NodeKind::Or);
    ASTERIA_CHECK(
        test, S::Parse(S::Literal("2>out 3<>file 4<&0 5>&- >>app >|force <<'A' <<-B\nx\nA\n\ty\n\tB\n"), program).Ok());
    auto redirect = program.Nodes[program.Nodes[program.Root].Left].Redirects;
    constexpr S::TokenKind kinds[]{S::TokenKind::Great,    S::TokenKind::ReadWrite, S::TokenKind::LessAnd,
                                   S::TokenKind::GreatAnd, S::TokenKind::Append,    S::TokenKind::Clobber,
                                   S::TokenKind::Here,     S::TokenKind::HereStrip};
    Size index{0U};
    for (; redirect != 0U; redirect = program.Redirects[redirect].Next)
    {
        ASTERIA_CHECK(test, index < 8U);
        if (index < 8U)
        {
            ASTERIA_CHECK_EQ(test, program.Redirects[redirect].Kind, kinds[index]);
        }
        ++index;
    }
    ASTERIA_CHECK_EQ(test, index, 8U);
    const char nul[]{'a', '\0', 'b'};
    ASTERIA_CHECK_EQ(test, S::Parse({nul, 3U}, program).Code, S::Error::NulByte);
    const char invalid[]{static_cast<char>(0xC0U), static_cast<char>(0xAFU)};
    ASTERIA_CHECK_EQ(test, S::Parse({invalid, 2U}, program).Code, S::Error::InvalidUtf8);
    for (Size i{0U}; i < sizeof(capacity); ++i)
    {
        capacity[i] = 'a';
    }
    ASTERIA_CHECK_EQ(test, S::Parse({capacity, sizeof(capacity)}, program).Code, S::Error::InputTooLarge);
    ASTERIA_CHECK(test, S::Parse({capacity, S::InputLimit}, program).Ok());
    for (Size i{0U}; i < 70U; ++i)
    {
        capacity[i] = '(';
        capacity[140U - i - 1U] = ')';
    }
    capacity[70U] = ':';
    ASTERIA_CHECK_EQ(test, S::Parse({capacity, 140U}, program).Code, S::Error::Depth);
    for (Size i{0U}; i < S::InputLimit; ++i)
    {
        capacity[i] = i % 2U == 0U ? 'a' : ' ';
    }
    ASTERIA_CHECK_EQ(test, S::Parse({capacity, S::InputLimit}, program).Code, S::Error::TokenCapacity);
    return test.Finish();
}
