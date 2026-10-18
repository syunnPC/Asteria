// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_OUTLINE_HPP_INCLUDED
#define ASTERIA_USER_FONT_OUTLINE_HPP_INCLUDED
#include "User/Font/Font.hpp"
namespace Asteria::User::Font
{
struct Point
{
    Int64 X{};
    Int64 Y{};
};
enum class CommandKind : UInt8
{
    Move,
    Line,
    Quadratic,
    Cubic,
    Close
};
struct Command
{
    CommandKind Kind{};
    Point A{};
    Point B{};
    Point C{};
};
struct Outline
{
    Command* Commands{};
    UInt32 Capacity{};
    UInt32 Count{};
    Int64 CharstringWidth{};
};
struct ProgramFrame
{
    Bytes Program{};
    Size At{};
};
struct Type2Workspace
{
    Int64 Operands[MaximumOperands]{};
    Int64 Transient[MaximumTransient]{};
    ProgramFrame Frames[MaximumSubroutineDepth + 1U]{};
    UInt32 Count{};
    UInt32 Depth{};
    UInt32 Stems{};
    UInt32 Tokens{};
    UInt32 Random{1U};
    Point Current{};
    Int64 Width{};
    Int64 Nominal{};
    bool WidthSeen{};
    bool Path{};
};
struct RawPoint
{
    Point Position{};
    UInt8 Flags{};
};
struct CompositeFrame
{
    Bytes Data{};
    Size At{};
    UInt32 Start{};
    UInt32 ChildStart{};
    Int64 A{One};
    Int64 B{};
    Int64 C{};
    Int64 D{One};
    Int64 X{};
    Int64 Y{};
    UInt16 Flags{};
    UInt16 Arg1{};
    UInt16 Arg2{};
    UInt16 Glyph{};
    bool Waiting{};
    bool Instructions{};
};
struct OutlineWorkspace
{
    Type2Workspace Type2{};
    RawPoint Points[MaximumPoints]{};
    UInt32 Ends[MaximumPoints]{};
    CompositeFrame Composites[MaximumCompositeDepth]{};
    UInt32 PointCount{};
    UInt32 ContourCount{};
    UInt32 Components{};
    UInt32 Depth{};
};
/// #5177 sections 3–4 and Appendix B. Workspace must not alias output/programs.
[[nodiscard]] Error InterpretType2(Bytes program, const Index& local, const Index& global, Int64 defaultWidth,
                                   Int64 nominalWidth, Type2Workspace& workspace, Outline& output) noexcept;
/// Quadratics stay quadratic; CFF curves stay cubic. Hinting is omitted in v1.
[[nodiscard]] Error LoadOutline(const Font& font, UInt16 glyph, OutlineWorkspace& workspace, Outline& output) noexcept;
} // namespace Asteria::User::Font
#endif
