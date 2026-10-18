// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SHELL_SHELL_HPP_INCLUDED
#define ASTERIA_USER_SHELL_SHELL_HPP_INCLUDED

#include "Foundation/Types.hpp"

// And its: this library consumes terminal bytes; the owning application binds Endpoints.
namespace Asteria::User::Shell
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::Int64;
inline constexpr Size InputLimit{8192U};
inline constexpr Size SourceStorageLimit{InputLimit * 2U};
static_assert(SourceStorageLimit <= 65535U);
inline constexpr Size TokenLimit{512U};
inline constexpr Size NodeLimit{512U};
inline constexpr Size WordLimit{512U};
inline constexpr Size RedirectLimit{128U};
inline constexpr Size DepthLimit{32U};
inline constexpr Size ScriptLimit{8U};
static_assert(ScriptLimit > 1U);
inline constexpr Size ExpansionLimit{8192U};
inline constexpr Size FieldLimit{64U};
inline constexpr Size VariableLimit{64U};
inline constexpr Size ValueLimit{1024U};
inline constexpr Size NameLimit{64U};
inline constexpr Size NumberLimit{32U};
inline constexpr Size FunctionLimit{16U};
inline constexpr Size CommandRedirectLimit{16U};
inline constexpr Size AssignmentLimit{16U};
inline constexpr UInt32 StepLimit{100000U};
static_assert(ExpansionLimit <= 65535U && ValueLimit <= 65535U && NameLimit <= 65535U);
static_assert(InputLimit <= 65535U && NodeLimit <= 65535U && WordLimit <= 65535U);

struct Text
{
    const char* Data{nullptr};
    Size Length{0U};
};
// Measure a C string up to InputLimit+1 bytes; oversized strings subsequently
// fail Parse/Expand rather than causing an unbounded scan.
[[nodiscard]] Text Literal(const char* text) noexcept;
[[nodiscard]] bool Equal(Text left, Text right) noexcept;
struct Buffer
{
    char* Data;
    Size Capacity;
    Size Length{0U};
    [[nodiscard]] bool Append(Text text) noexcept;
    [[nodiscard]] bool Put(char byte) noexcept;
    [[nodiscard]] Text View() const noexcept
    {
        return {Data, Length};
    }
};
enum class Error : Foundation::UInt8
{
    None,
    InputTooLarge,
    InvalidUtf8,
    NulByte,
    TokenCapacity,
    NodeCapacity,
    WordCapacity,
    RedirectCapacity,
    Syntax,
    UnterminatedQuote,
    UnterminatedExpansion,
    MissingHereDocument,
    Depth,
    ExpansionCapacity,
    FieldCapacity,
    VariableCapacity,
    InvalidName,
    ValueCapacity,
    FunctionCapacity,
    Arithmetic,
    ParameterUnset,
    Host,
    Steps,
    InvalidProgram
};
struct Result
{
    Error Code{Error::None};
    Size Offset{0U};
    int Status{0};
    [[nodiscard]] bool Ok() const noexcept
    {
        return Code == Error::None;
    }
};
[[nodiscard]] Text ErrorMessage(Error code) noexcept;
struct Span
{
    UInt16 Offset{0U};
    UInt16 Length{0U};
};
enum class TokenKind : Foundation::UInt8
{
    End,
    Word,
    Newline,
    Semi,
    Amp,
    Pipe,
    And,
    Or,
    DoubleSemi,
    SemiAnd,
    LeftParen,
    RightParen,
    Less,
    Great,
    Append,
    Here,
    HereStrip,
    LessAnd,
    GreatAnd,
    ReadWrite,
    Clobber
};
struct Token
{
    TokenKind Kind{TokenKind::End};
    Span Spelling{};
    Span Body{};
    Span Original{};
    bool QuotedDelimiter{false};
};
enum class NodeKind : Foundation::UInt8
{
    Simple,
    List,
    And,
    Or,
    Pipeline,
    Background,
    Subshell,
    Group,
    If,
    While,
    Until,
    For,
    Case,
    CaseItem,
    Function,
    Negate
};
struct Word
{
    Span Spelling{};
    UInt16 Next{0U};
    bool Assignment{false};
};
struct Redirect
{
    TokenKind Kind{TokenKind::Less};
    UInt16 Descriptor{0U};
    Span Target{};
    Span Body{};
    UInt16 Next{0U};
    bool Quoted{false};
};
struct Node
{
    NodeKind Kind{NodeKind::Simple};
    UInt16 Left{0U};
    UInt16 Right{0U};
    UInt16 Third{0U};
    UInt16 Next{0U};
    UInt16 Words{0U};
    UInt16 Redirects{0U};
    Span Source{};
    bool ImplicitParameters{false};
};
struct Program
{
    char Source[SourceStorageLimit + 1U]{};
    Size Length{0U};
    Size StorageLength{0U};
    Token Tokens[TokenLimit + 1U]{};
    Node Nodes[NodeLimit + 1U]{};
    Word Words[WordLimit + 1U]{};
    Redirect Redirects[RedirectLimit + 1U]{};
    Size TokenCount{0U};
    UInt16 NodeCount{0U};
    UInt16 WordCount{0U};
    UInt16 RedirectCount{0U};
    UInt16 Root{0U};
    Result Diagnostic{};
    [[nodiscard]] Text View(Span span) const noexcept
    {
        return {Source + span.Offset, span.Length};
    }
};
[[nodiscard]] Result Parse(Text source, Program& output) noexcept;

struct Variable
{
    char Name[NameLimit + 1U]{};
    char Value[ValueLimit + 1U]{};
    UInt16 NameLength{0U};
    UInt16 ValueLength{0U};
    bool Present{false};
    bool Exported{false};
    bool NoValue{false};
};
struct Value
{
    char Bytes[ValueLimit + 1U]{};
    UInt16 Length{0U};
};
struct State
{
    Variable Variables[VariableLimit]{};
    Value Parameters[FieldLimit]{};
    Size ParameterCount{0U};
    Value Name{};
    UInt64 ProcessId{1U};
    UInt64 BackgroundId{0U};
    int LastStatus{0};
    bool Nounset{false};
    bool NoGlob{false};
    bool Errexit{false};
    bool NoExecute{false};
    bool AllExport{false};
    bool NoClobber{false};
    bool Trace{false};
    bool Pipefail{false};
    bool ExitRequested{false};
};
struct Fields
{
    char Bytes[ExpansionLimit]{};
    Span Items[FieldLimit]{};
    Size Used{0U};
    Size Count{0U};
    [[nodiscard]] bool Add(Text value) noexcept;
    [[nodiscard]] Text At(Size index) const noexcept
    {
        return {Bytes + Items[index].Offset, Items[index].Length};
    }
};
struct FileInfo
{
    bool Exists{false};
    bool Directory{false};
    bool Regular{false};
    bool Readable{false};
    bool Writable{false};
    bool Executable{false};
    UInt64 Bytes{0U};
    bool Block{false};
    bool Character{false};
    bool Fifo{false};
    bool Socket{false};
    bool SymbolicLink{false};
    bool SetUserId{false};
    bool SetGroupId{false};
    UInt64 Device{0U};
    UInt64 Identity{0U};
    Int64 ModifiedSeconds{0};
    UInt32 ModifiedNanoseconds{0U};
};
struct Filesystem
{
    void* Context{nullptr};
    // Return UTF-8 basenames, excluding dot and dot-dot; no pattern matching
    // in the host. The core traverses slash components and sorts matches.
    bool (*ListDirectory)(void*, Text, Fields&) noexcept {nullptr};
    bool (*Home)(void*, Text, Buffer&) noexcept {nullptr};
    bool (*ChangeDirectory)(void*, Text) noexcept {nullptr};
    bool (*WorkingDirectory)(void*, Buffer&) noexcept {nullptr};
    bool (*ReadFile)(void*, Text, Buffer&) noexcept {nullptr};
    // Attributes follow the final symlink, except SymbolicLink, which reports
    // the final entry itself (including a dangling link with Exists=false).
    bool (*Inspect)(void*, Text, FileInfo&) noexcept {nullptr};
};
struct ExpandedRedirect
{
    TokenKind Kind;
    UInt16 Descriptor;
    Text Target;
    bool NoClobber{false};
};
class Engine;
enum class Isolation : Foundation::UInt8
{
    Subshell,
    Pipeline,
    Background
};
struct ProcessHost
{
    void* Context{nullptr};
    bool (*Launch)(void*, const Fields&, const State&, int&) noexcept {nullptr};
    bool (*Write)(void*, UInt16, Text) noexcept {nullptr};
    bool (*Read)(void*, UInt16, Buffer&, bool&) noexcept {nullptr};
    // Begin is atomic on failure; End restores descriptors after any outcome.
    bool (*BeginRedirects)(void*, const ExpandedRedirect*, Size, UInt64&) noexcept {nullptr};
    void (*EndRedirects)(void*, UInt64) noexcept {nullptr};
    bool (*Isolate)(void*, Isolation, Engine&, const Program&, UInt16, int&, UInt64&) noexcept {nullptr};
    bool (*Capture)(void*, Engine&, Text, Buffer&, int&) noexcept {nullptr};
    bool (*Resolve)(void*, Text, const State&, Buffer&) noexcept {nullptr};
    bool (*IsTerminal)(void*, UInt16) noexcept {nullptr};
};
struct ExpansionScratch
{
    char Bytes[ExpansionLimit]{};
    Foundation::UInt8 Flags[ExpansionLimit]{};
    Size Length{0U};
    Size Boundaries[FieldLimit]{};
    Size BoundaryCount{0U};
    bool KeepEmpty{false};
    bool Split{true};
    Size EmptyMarkers[FieldLimit]{};
    Size MarkerCount{0U};
    char Number[NumberLimit]{};
    char ParameterName[NameLimit + 1U]{};
    Fields Temporary{};
    Fields Paths{};
    Fields Entries{};
    char PathBytes[ExpansionLimit]{};
};
struct PrintFormat
{
    Size Width{0U};
    Size Precision{0U};
    char Conversion{0};
    bool Left{false};
    bool Zero{false};
    bool Plus{false};
    bool Blank{false};
    bool Alternate{false};
    bool PrecisionSet{false};
};
struct ExecutionScratch
{
    PrintFormat Format{};
    Fields Arguments{};
    Fields Targets{};
    ExpandedRedirect Redirects[CommandRedirectLimit]{};
    Variable Saved[AssignmentLimit]{};
    Size SavedIndices[AssignmentLimit]{};
    Size SavedCount{0U};
    bool HasSubstitution{false};
    int SubstitutionStatus{0};
    Value Parameters[FieldLimit]{};
    Size ParameterCount{0U};
    char TextBytes[InputLimit + 1U]{};
};
struct Function
{
    char Name[NameLimit + 1U]{};
    Size NameLength{0U};
    char Source[InputLimit + 1U]{};
    Size Length{0U};
    bool Present{false};
};
class Engine final
{
public:
    explicit Engine(Filesystem filesystem = {}, ProcessHost host = {}) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    State Environment{};
    [[nodiscard]] Result Set(Text name, Text value, bool exported = false) noexcept;
    [[nodiscard]] Text Get(Text name, bool& present) const noexcept;
    [[nodiscard]] Result Unset(Text name) noexcept;
    [[nodiscard]] Result Run(Text source) noexcept;
    [[nodiscard]] Result Execute(const Program& program, UInt16 node = 0U) noexcept;
    [[nodiscard]] Result Expand(Text word, Fields& output, bool split = true, bool glob = true,
                                bool assignment = false) noexcept;
    // Host uses this to initialize an isolated child. Its filesystem and host
    // bindings are chosen by its constructor, not copied from the parent.
    void CopyStateFrom(const Engine& parent) noexcept;
    [[nodiscard]] bool Match(Text pattern, Text value, const Foundation::UInt8* quotes = nullptr) noexcept;

private:
    Filesystem m_filesystem;
    ProcessHost m_host;
    Program m_programs[ScriptLimit]{};
    Function m_functions[FunctionLimit]{};
    ExpansionScratch m_expansions[ScriptLimit]{};
    ExecutionScratch m_execution[DepthLimit]{};
    Size m_scriptDepth{0U};
    Size m_expansionDepth{0U};
    Size m_executionDepth{0U};
    Size m_functionDepth{0U};
    Size m_sourceDepth{0U};
    Size m_suppression{0U};
    Size m_childDepth{0U};
    UInt32 m_steps{0U};
    bool m_return{false};
    Result m_error{};
    [[nodiscard]] bool Fail(Error error, Size offset = 0U) noexcept;
    [[nodiscard]] bool Step() noexcept;
    [[nodiscard]] bool ValidateState() noexcept;
    [[nodiscard]] Size Find(Text name) const noexcept;
    [[nodiscard]] int Evaluate(const Program& program, UInt16 node) noexcept;
    [[nodiscard]] int Invoke(ExecutionScratch& frame) noexcept;
    [[nodiscard]] int Simple(const Program& program, const Node& node, ExecutionScratch& frame) noexcept;
    [[nodiscard]] bool PrintInteger(Text operand, const PrintFormat& format, Buffer& output, int& status) noexcept;
    [[nodiscard]] int Printf(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int PrintWorkingDirectory(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int ChangeDirectory(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] bool DirectoryOperand(Text operand, bool& print, ExecutionScratch& frame) noexcept;
    [[nodiscard]] bool NormalizeDirectory(Buffer& path) noexcept;
    [[nodiscard]] int ExportBuiltin(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int UnsetBuiltin(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int SetBuiltin(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int PrintOptions(bool reusable) noexcept;
    [[nodiscard]] int SourceBuiltin(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int TypeBuiltin(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int ReadBuiltin(const Fields& args, ExecutionScratch& frame) noexcept;
    [[nodiscard]] int Builtin(const Fields& args, ExecutionScratch& frame, bool& found) noexcept;
    [[nodiscard]] bool Substitution(Text word, Size& cursor, ExpansionScratch& out, bool quoted) noexcept;
    [[nodiscard]] bool EmitField(ExpansionScratch& out, Size start, Size end, Fields& output, bool glob) noexcept;
    [[nodiscard]] bool ExpandInto(Text word, ExpansionScratch& out, bool quoted, bool here = false,
                                  bool assignment = false) noexcept;
    [[nodiscard]] bool Parameter(Text expression, ExpansionScratch& out, bool quoted) noexcept;
    [[nodiscard]] bool Append(ExpansionScratch& out, Text value, Foundation::UInt8 flags) noexcept;
    [[nodiscard]] bool Arithmetic(Text text, Int64& result) noexcept;
    [[nodiscard]] bool Pathnames(Text pattern, const Foundation::UInt8* flags, ExpansionScratch& scratch) noexcept;
    [[nodiscard]] bool Write(Text text, UInt16 fd = 1U) noexcept;
    [[nodiscard]] bool Redirects(const Program& program, UInt16 first, ExecutionScratch& frame,
                                 UInt64& cookie) noexcept;
    [[nodiscard]] Text Special(Text name, ExpansionScratch& scratch, bool& present) noexcept;
    [[nodiscard]] int Nested(Text source) noexcept;
    [[nodiscard]] bool SaveAssignment(ExecutionScratch& frame, Text name) noexcept;
    void RestoreAssignments(ExecutionScratch& frame) noexcept;
};
} // Namespace Asteria::User::Shell
#endif
