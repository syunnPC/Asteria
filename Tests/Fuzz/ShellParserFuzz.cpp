// SPDX-License-Identifier: Apache-2.0
#include "User/Shell/Shell.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace
{
namespace S = Asteria::User::Shell;
static S::Program first{};
static S::Program second{};
void Require(bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    const S::Text input{reinterpret_cast<const char*>(data), size};
    const auto result = S::Parse(input, first);
    const auto replay = S::Parse(input, second);
    Require(result.Code == replay.Code && result.Offset == replay.Offset && result.Offset <= size);
    if (!result.Ok())
    {
        return 0;
    }
    Require(first.Length == size && first.Root <= first.NodeCount && first.NodeCount <= S::NodeLimit &&
            first.WordCount <= S::WordLimit && first.RedirectCount <= S::RedirectLimit &&
            first.TokenCount <= S::TokenLimit);
    Require(first.NodeCount == second.NodeCount && first.WordCount == second.WordCount &&
            first.RedirectCount == second.RedirectCount);
    for (std::size_t i{1U}; i <= first.NodeCount; ++i)
    {
        const auto& n = first.Nodes[i];
        const auto& r = second.Nodes[i];
        Require(n.Kind == r.Kind && n.Left == r.Left && n.Right == r.Right && n.Third == r.Third && n.Next == r.Next &&
                n.Words == r.Words && n.Redirects == r.Redirects);
        Require(n.Left <= first.NodeCount && n.Right <= first.NodeCount && n.Third <= first.NodeCount &&
                n.Next <= first.NodeCount && n.Words <= first.WordCount && n.Redirects <= first.RedirectCount);
        Require(static_cast<std::size_t>(n.Source.Offset) + n.Source.Length <= size);
    }
    for (std::size_t i{1U}; i <= first.WordCount; ++i)
    {
        const auto& w = first.Words[i];
        Require(w.Next <= first.WordCount &&
                static_cast<std::size_t>(w.Spelling.Offset) + w.Spelling.Length <= first.StorageLength);
    }
    for (std::size_t i{1U}; i <= first.RedirectCount; ++i)
    {
        const auto& r = first.Redirects[i];
        Require(r.Next <= first.RedirectCount &&
                static_cast<std::size_t>(r.Target.Offset) + r.Target.Length <= first.StorageLength &&
                static_cast<std::size_t>(r.Body.Offset) + r.Body.Length <= size);
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr const char* seeds[]{"",
                                  "echo hello",
                                  "a && b || c | d & e; f",
                                  "if a; then b; elif c; then d; else e; fi",
                                  "while a; do b; done",
                                  "until a; do b; done",
                                  "for x in a b; do :; done",
                                  "case x in (a|b) c;; *) ;; esac",
                                  "f() { (echo \"${v:-$(printf '%s' \"$((1+2))\")}\"); } >out",
                                  "cat <<'A' <<-B\n$literal\nA\n\t${v}\n\tB\n",
                                  "echo '日本語'",
                                  "echo ab\\\ncd",
                                  "echo $'a\\nb'",
                                  "2>&1 3<&- 4<>file >>append >|force"};
    for (const char* seed : seeds)
    {
        const auto text = S::Literal(seed);
        emit(reinterpret_cast<const std::uint8_t*>(text.Data), text.Length);
    }
}
