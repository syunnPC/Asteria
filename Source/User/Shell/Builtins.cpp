// SPDX-License-Identifier: Apache-2.0

#include "User/Shell/Internal.hpp"
namespace Asteria::User::Shell
{
using Detail::Blank;
using Detail::BuiltinName;
using Detail::Decimal;
using Detail::Digit;
using Detail::IfsWidth;
using Detail::Integer;
using Detail::Is;
using Detail::Name;
using Detail::Quoted;
using Detail::ScalarBytes;
using Detail::Slice;
using Detail::Space;
using Detail::SpecialBuiltin;
using Detail::Splittable;
using Detail::UnsignedNumber;
using Detail::Validate;
namespace
{
[[nodiscard]] bool Signed(Text text, Int64& value) noexcept
{
    bool negative{false};
    if (text.Length != 0U && (text.Data[0U] == '+' || text.Data[0U] == '-'))
    {
        negative = text.Data[0U] == '-';
        text = Slice(text, 1U, text.Length - 1U);
    }
    UInt64 magnitude{0U};
    if (!Decimal(text, magnitude) || magnitude > (negative ? UInt64{1U} << 63U : (UInt64{1U} << 63U) - 1U))
    {
        return false;
    }
    value = static_cast<Int64>(negative ? UInt64{0U} - magnitude : magnitude);
    return true;
}
[[nodiscard]] int Compare(Text a, Text b) noexcept
{
    const Size n = a.Length < b.Length ? a.Length : b.Length;
    for (Size i{0U}; i < n; ++i)
    {
        if (a.Data[i] != b.Data[i])
        {
            return static_cast<Foundation::UInt8>(a.Data[i]) < static_cast<Foundation::UInt8>(b.Data[i]) ? -1 : 1;
        }
    }
    return a.Length == b.Length ? 0 : (a.Length < b.Length ? -1 : 1);
}
class TestExpression final
{
public:
    TestExpression(const Fields& args, Size end, const Filesystem& filesystem, const ProcessHost& host) noexcept
        : m_args{args}, m_end{end}, m_filesystem{filesystem}, m_host{host}
    {
    }
    // Preserve small IA-64 frames when optimization considers combining
    // the argument-count evaluator with filesystem inspection temporaries.
    [[nodiscard, gnu::noinline]] int Run() noexcept
    {
        // IEEE Std 1003.1-2024, test OPERANDS: count before precedence.
        Size first{1U};
        Size count = m_end - first;
        bool negate{false};
        while ((count == 3U || count == 4U) && Is(m_args.At(first), "!") &&
               !(count == 3U && BinaryOperator(m_args.At(first + 1U))))
        {
            ++first;
            --count;
            negate = !negate;
        }
        bool value{false};
        bool valid{true};
        if (count == 0U)
        {
            value = false;
        }
        else if (count == 1U)
        {
            value = m_args.At(first).Length != 0U;
        }
        else if (count == 2U && Is(m_args.At(first), "!"))
        {
            value = m_args.At(first + 1U).Length == 0U;
        }
        else if (count == 2U)
        {
            value = Unary(m_args.At(first), m_args.At(first + 1U), valid);
        }
        else if (count == 3U && BinaryOperator(m_args.At(first + 1U)))
        {
            value = Binary(m_args.At(first), m_args.At(first + 1U), m_args.At(first + 2U), valid);
        }
        else
        {
            valid = false;
        } // Unspecified argument shapes are errors.
        return valid ? ((value != negate) ? 0 : 1) : 2;
    }

private:
    const Fields& m_args;
    Size m_end;
    const Filesystem& m_filesystem;
    const ProcessHost& m_host;
    [[nodiscard]] static bool BinaryOperator(Text op) noexcept
    {
        return Is(op, "=") || Is(op, "!=") || Is(op, "<") || Is(op, ">") || Is(op, "-eq") || Is(op, "-ne") ||
               Is(op, "-lt") || Is(op, "-le") || Is(op, "-gt") || Is(op, "-ge") || Is(op, "-ef") || Is(op, "-nt") ||
               Is(op, "-ot");
    }
    [[nodiscard]] bool Binary(Text left, Text op, Text right, bool& valid) noexcept
    {
        if (Is(op, "="))
        {
            return Equal(left, right);
        }
        if (Is(op, "!="))
        {
            return !Equal(left, right);
        }
        if (Is(op, "<") || Is(op, ">"))
        {
            return Is(op, "<") ? Compare(left, right) < 0 : Compare(left, right) > 0;
        }
        if (Is(op, "-ef") || Is(op, "-nt") || Is(op, "-ot"))
        {
            return Files(left, op, right);
        }
        Int64 a{0};
        Int64 b{0};
        if (!Signed(left, a) || !Signed(right, b))
        {
            valid = false;
            return false;
        }
        if (Is(op, "-eq"))
        {
            return a == b;
        }
        if (Is(op, "-ne"))
        {
            return a != b;
        }
        if (Is(op, "-lt"))
        {
            return a < b;
        }
        if (Is(op, "-le"))
        {
            return a <= b;
        }
        if (Is(op, "-gt"))
        {
            return a > b;
        }
        return a >= b;
    }
    [[nodiscard, gnu::noinline]] bool Files(Text left, Text op, Text right) noexcept
    {
        FileInfo a{};
        FileInfo b{};
        if (m_filesystem.Inspect == nullptr)
        {
            return false;
        }
        if (!m_filesystem.Inspect(m_filesystem.Context, left, a))
        {
            a.Exists = false;
        }
        if (!m_filesystem.Inspect(m_filesystem.Context, right, b))
        {
            b.Exists = false;
        }
        if (Is(op, "-ef"))
        {
            return a.Exists && b.Exists && a.Device == b.Device && a.Identity == b.Identity;
        }
        if (Is(op, "-nt"))
        {
            return a.Exists &&
                   (!b.Exists || a.ModifiedSeconds > b.ModifiedSeconds ||
                    (a.ModifiedSeconds == b.ModifiedSeconds && a.ModifiedNanoseconds > b.ModifiedNanoseconds));
        }
        return b.Exists && (!a.Exists || a.ModifiedSeconds < b.ModifiedSeconds ||
                            (a.ModifiedSeconds == b.ModifiedSeconds && a.ModifiedNanoseconds < b.ModifiedNanoseconds));
    }
    [[nodiscard, gnu::noinline]] bool Unary(Text op, Text operand, bool& valid) noexcept
    {
        if (Is(op, "-n"))
        {
            return operand.Length != 0U;
        }
        if (Is(op, "-z"))
        {
            return operand.Length == 0U;
        }
        if (Is(op, "-t"))
        {
            UInt64 fd{0U};
            return Decimal(operand, fd) && fd <= 65535U && m_host.IsTerminal != nullptr &&
                   m_host.IsTerminal(m_host.Context, static_cast<UInt16>(fd));
        }
        if (!(Is(op, "-e") || Is(op, "-f") || Is(op, "-d") || Is(op, "-r") || Is(op, "-w") || Is(op, "-x") ||
              Is(op, "-s") || Is(op, "-b") || Is(op, "-c") || Is(op, "-p") || Is(op, "-S") || Is(op, "-h") ||
              Is(op, "-L") || Is(op, "-u") || Is(op, "-g")))
        {
            valid = false;
            return false;
        }
        FileInfo info{};
        if (m_filesystem.Inspect == nullptr || !m_filesystem.Inspect(m_filesystem.Context, operand, info))
        {
            return false;
        }
        if (Is(op, "-h") || Is(op, "-L"))
        {
            return info.SymbolicLink;
        }
        return info.Exists &&
               (Is(op, "-e") || (Is(op, "-f") && info.Regular) || (Is(op, "-d") && info.Directory) ||
                (Is(op, "-r") && info.Readable) || (Is(op, "-w") && info.Writable) ||
                (Is(op, "-x") && info.Executable) || (Is(op, "-s") && info.Bytes != 0U) ||
                (Is(op, "-b") && info.Block) || (Is(op, "-c") && info.Character) || (Is(op, "-p") && info.Fifo) ||
                (Is(op, "-S") && info.Socket) || (Is(op, "-u") && info.SetUserId) || (Is(op, "-g") && info.SetGroupId));
    }
};
[[nodiscard]] bool Escape(Text text, Size& cursor, char& byte, bool& stop, bool operand) noexcept
{
    if (cursor >= text.Length)
    {
        byte = '\\';
        return true;
    }
    byte = text.Data[cursor++];
    switch (byte)
    {
    case 'a':
        byte = '\a';
        return true;
    case 'b':
        byte = '\b';
        return true;
    case 'f':
        byte = '\f';
        return true;
    case 'n':
        byte = '\n';
        return true;
    case 'r':
        byte = '\r';
        return true;
    case 't':
        byte = '\t';
        return true;
    case 'v':
        byte = '\v';
        return true;
    case '\\':
        return true;
    case 'c':
        if (operand)
        {
            stop = true;
            return true;
        }
        return false;
    default:
        if (byte < '0' || byte > '7' || (operand && byte != '0'))
        {
            return false;
        }
        unsigned number = operand ? 0U : static_cast<unsigned>(byte - '0');
        for (unsigned i{0U};
             i < (operand ? 3U : 2U) && cursor < text.Length && text.Data[cursor] >= '0' && text.Data[cursor] <= '7';
             ++i)
        {
            number = number * 8U + static_cast<unsigned>(text.Data[cursor++] - '0');
        }
        byte = static_cast<char>(static_cast<Foundation::UInt8>(number & 255U));
        return true;
    }
}
} // Namespace
bool Engine::PrintInteger(const Text operand, const PrintFormat& format, Buffer& output, int& status) noexcept
{
    Int64 number{0};
    if (operand.Length != 0U && (operand.Data[0U] == '\'' || operand.Data[0U] == '"'))
    {
        if (operand.Length < 2U)
        {
            return false;
        }
        const auto c = static_cast<Foundation::UInt8>(operand.Data[1U]);
        UInt32 scalar = c;
        const Size n = ScalarBytes(operand, 1U);
        if (n > 1U)
        {
            scalar = c & (n == 2U ? 0x1FU : (n == 3U ? 0x0FU : 0x07U));
            for (Size j{1U}; j < n; ++j)
            {
                scalar = (scalar << 6U) | (static_cast<Foundation::UInt8>(operand.Data[1U + j]) & 0x3FU);
            }
        }
        number = scalar;
    }
    else if (operand.Length != 0U)
    {
        UInt64 parsed{0U};
        if (!Integer(operand, parsed))
        {
            status = 1;
            static_cast<void>(Write(Literal("printf: invalid integer\n"), 2U));
        }
        number = static_cast<Int64>(parsed);
    }
    char digits[NumberLimit]{};
    const bool signedValue = format.Conversion == 'd' || format.Conversion == 'i';
    const bool negative = signedValue && number < 0;
    const unsigned base =
        format.Conversion == 'o' ? 8U : (format.Conversion == 'x' || format.Conversion == 'X' ? 16U : 10U);
    const UInt64 magnitude = negative ? UInt64{0U} - static_cast<UInt64>(number) : static_cast<UInt64>(number);
    Text rendered = UnsignedNumber(magnitude, digits, base);
    if (format.PrecisionSet && format.Precision == 0U && magnitude == 0U)
    {
        rendered.Length = 0U;
    }
    if (negative)
    {
        static_cast<void>(output.Put('-'));
    }
    else if (signedValue && (format.Plus || format.Blank))
    {
        static_cast<void>(output.Put(format.Plus ? '+' : ' '));
    }
    if (format.Alternate && base == 8U && (rendered.Length == 0U || rendered.Data[0U] != '0'))
    {
        static_cast<void>(output.Put('0'));
    }
    if (format.Alternate && base == 16U && magnitude != 0U)
    {
        static_cast<void>(output.Append(format.Conversion == 'X' ? Literal("0X") : Literal("0x")));
    }
    const Size sign = output.Length;
    Size pad = format.Precision > rendered.Length ? format.Precision - rendered.Length : 0U;
    if (format.Zero && !format.Left && !format.PrecisionSet && format.Width > sign + rendered.Length)
    {
        pad = format.Width - sign - rendered.Length;
    }
    for (Size j{0U}; j < pad; ++j)
    {
        if (!output.Put('0'))
        {
            return false;
        }
    }
    for (Size j{0U}; j < rendered.Length; ++j)
    {
        char c = rendered.Data[j];
        if (format.Conversion == 'X' && c >= 'a' && c <= 'f')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (!output.Put(c))
        {
            return false;
        }
    }
    return true;
}
int Engine::Printf(const Fields& args, ExecutionScratch& frame) noexcept
{
    if (args.Count < 2U)
    {
        return 2;
    }
    const Text format = args.At(1U);
    Size argument{2U};
    bool stop{false};
    int status{0};
    do
    {
        const Size before = argument;
        const Size group = argument;
        bool numbered{false};
        bool unnumbered{false};
        for (Size i{0U}; i < format.Length && !stop && Step();)
        {
            char byte = format.Data[i++];
            if (byte == '\\')
            {
                if (!Escape(format, i, byte, stop, false))
                {
                    return 2;
                }
                if (!Write({&byte, 1U}))
                {
                    return 1;
                }
                continue;
            }
            if (byte != '%')
            {
                if (!Write({&byte, 1U}))
                {
                    return 1;
                }
                continue;
            }
            if (i == format.Length)
            {
                return 2;
            }
            if (format.Data[i] == '%')
            {
                ++i;
                if (!Write(Literal("%")))
                {
                    return 1;
                }
                continue;
            }
            Size operandIndex = argument;
            const Size indexStart = i;
            Size numberIndex{0U};
            while (i < format.Length && Digit(format.Data[i]))
            {
                numberIndex = numberIndex * 10U + static_cast<Size>(format.Data[i++] - '0');
                if (numberIndex > ExpansionLimit)
                {
                    return 2;
                }
            }
            if (i < format.Length && format.Data[i] == '$')
            {
                if (numberIndex == 0U || unnumbered)
                {
                    return 2;
                }
                numbered = true;
                ++i;
                operandIndex = group + numberIndex - 1U;
            }
            else
            {
                if (numbered)
                {
                    return 2;
                }
                unnumbered = true;
                i = indexStart;
            }
            PrintFormat& spec = frame.Format;
            spec = {};
            while (i < format.Length)
            {
                const char flag = format.Data[i];
                if (flag == '-')
                {
                    spec.Left = true;
                }
                else if (flag == '0')
                {
                    spec.Zero = true;
                }
                else if (flag == '+')
                {
                    spec.Plus = true;
                }
                else if (flag == ' ')
                {
                    spec.Blank = true;
                }
                else if (flag == '#')
                {
                    spec.Alternate = true;
                }
                else
                {
                    break;
                }
                ++i;
            }

            while (i < format.Length && Digit(format.Data[i]))
            {
                spec.Width = spec.Width * 10U + static_cast<Size>(format.Data[i++] - '0');
                if (spec.Width > ExpansionLimit)
                {
                    return 2;
                }
            }

            if (i < format.Length && format.Data[i] == '.')
            {
                spec.PrecisionSet = true;
                ++i;
                while (i < format.Length && Digit(format.Data[i]))
                {
                    spec.Precision = spec.Precision * 10U + static_cast<Size>(format.Data[i++] - '0');
                    if (spec.Precision > ExpansionLimit)
                    {
                        return 2;
                    }
                }
            }
            if (i == format.Length)
            {
                return 2;
            }
            spec.Conversion = format.Data[i++];
            const Text operand = operandIndex < args.Count ? args.At(operandIndex) : Text{};
            if (operandIndex < args.Count && operandIndex >= argument)
            {
                argument = operandIndex + 1U;
            }
            Buffer output{frame.TextBytes, InputLimit};
            if (spec.Conversion == 's' || spec.Conversion == 'c')
            {
                const Size length =
                    spec.Conversion == 'c'
                        ? (operand.Length != 0U ? 1U : 0U)
                        : (spec.PrecisionSet && spec.Precision < operand.Length ? spec.Precision : operand.Length);
                if (!output.Append(Slice(operand, 0U, length)))
                {
                    return 2;
                }
            }
            else if (spec.Conversion == 'b')
            {
                for (Size j{0U}; j < operand.Length && !stop && (!spec.PrecisionSet || output.Length < spec.Precision);)
                {
                    char c = operand.Data[j++];
                    if (c == '\\' && !Escape(operand, j, c, stop, true))
                    {
                        return 2;
                    }
                    if (!stop && !output.Put(c))
                    {
                        return 2;
                    }
                }
            }
            else if (spec.Conversion == 'd' || spec.Conversion == 'i' || spec.Conversion == 'u' ||
                     spec.Conversion == 'o' || spec.Conversion == 'x' || spec.Conversion == 'X')
            {
                if (!PrintInteger(operand, spec, output, status))
                {
                    return 2;
                }
            }
            else
            {
                return 2;
            }
            const Size pad = spec.Width > output.Length ? spec.Width - output.Length : 0U;
            if (!spec.Left)
            {
                for (Size j{0U}; j < pad; ++j)
                {
                    if (!Write(Literal(" ")))
                    {
                        return 1;
                    }
                }
            }
            if (!Write(output.View()))
            {
                return 1;
            }
            if (spec.Left)
            {
                for (Size j{0U}; j < pad; ++j)
                {
                    if (!Write(Literal(" ")))
                    {
                        return 1;
                    }
                }
            }
        }
        if (before == argument)
        {
            break;
        }
    } while (!stop && argument < args.Count && Step());
    return status;
}
int Engine::PrintWorkingDirectory(const Fields& args, ExecutionScratch& frame) noexcept
{
    bool physical{false};
    for (Size i{1U}; i < args.Count; ++i)
    {
        if (Is(args.At(i), "-P"))
        {
            physical = true;
        }
        else if (Is(args.At(i), "-L"))
        {
            physical = false;
        }
        else
        {
            return 2;
        }
    }
    bool present{false};
    const Text pwd = Get(Literal("PWD"), present);
    if (!physical && present && pwd.Length != 0U && pwd.Data[0U] == '/')
    {
        return Write(pwd) && Write(Literal("\n")) ? 0 : 1;
    }
    Buffer output{frame.TextBytes, InputLimit};
    if (m_filesystem.WorkingDirectory == nullptr || !m_filesystem.WorkingDirectory(m_filesystem.Context, output))
    {
        return 1;
    }
    return Write(output.View()) && Write(Literal("\n")) ? 0 : 1;
}
bool Engine::DirectoryOperand(Text operand, bool& print, ExecutionScratch& frame) noexcept
{
    // IEEE Std 1003.1-2024, cd DESCRIPTION steps 3-6 and STDOUT.
    frame.Targets.Count = 0U;
    frame.Targets.Used = 0U;
    Size firstEnd{0U};
    while (firstEnd < operand.Length && operand.Data[firstEnd] != '/')
    {
        ++firstEnd;
    }
    bool present{false};
    const Text search = Get(Literal("CDPATH"), present);
    if (present && operand.Data[0U] != '/' && !Is(Slice(operand, 0U, firstEnd), ".") &&
        !Is(Slice(operand, 0U, firstEnd), ".."))
    {
        if (m_filesystem.Inspect == nullptr)
        {
            return Fail(Error::Host);
        }
        for (Size start{0U}; start <= search.Length;)
        {
            Size end = start;
            while (end < search.Length && search.Data[end] != ':')
            {
                ++end;
            }
            Buffer candidate{frame.TextBytes, InputLimit};
            if (end != start && (!candidate.Append(Slice(search, start, end - start)) || !candidate.Put('/')))
            {
                return Fail(Error::ExpansionCapacity);
            }
            if (!candidate.Append(operand))
            {
                return Fail(Error::ExpansionCapacity);
            }
            FileInfo info{};
            if (m_filesystem.Inspect(m_filesystem.Context, candidate.View(), info) && info.Exists && info.Directory)
            {
                print = print || end != start;
                return frame.Targets.Add(candidate.View()) || Fail(Error::ExpansionCapacity);
            }
            if (end == search.Length)
            {
                break;
            }
            start = end + 1U;
        }
    }
    return frame.Targets.Add(operand) || Fail(Error::ExpansionCapacity);
}
bool Engine::NormalizeDirectory(Buffer& path) noexcept
{
    // Cd DESCRIPTION step 8: check the component before eliminating /...
    Size read{0U};
    Size write{0U};
    const bool absolute = path.Length != 0U && path.Data[0U] == '/';
    if (absolute)
    {
        path.Data[write++] = '/';
    }
    while (read < path.Length)
    {
        while (read < path.Length && path.Data[read] == '/')
        {
            ++read;
        }
        const Size start = read;
        while (read < path.Length && path.Data[read] != '/')
        {
            ++read;
        }
        const Text component{path.Data + start, read - start};
        if (component.Length == 0U || Is(component, "."))
        {
            continue;
        }
        if (Is(component, ".."))
        {
            if (write > (absolute ? 1U : 0U))
            {
                FileInfo info{};
                if (m_filesystem.Inspect == nullptr ||
                    !m_filesystem.Inspect(m_filesystem.Context, {path.Data, write}, info) || !info.Exists ||
                    !info.Directory)
                {
                    return false;
                }
                if (path.Data[write - 1U] == '/')
                {
                    --write;
                }
                while (write > (absolute ? 1U : 0U) && path.Data[write - 1U] != '/')
                {
                    --write;
                }
            }
            continue;
        }
        if (write != 0U && path.Data[write - 1U] != '/')
        {
            path.Data[write++] = '/';
        }
        for (Size j{0U}; j < component.Length; ++j)
        {
            path.Data[write++] = component.Data[j];
        }
    }
    if (write > 1U && path.Data[write - 1U] == '/')
    {
        --write;
    }
    path.Length = write;
    return true;
}
int Engine::ChangeDirectory(const Fields& args, ExecutionScratch& frame) noexcept
{
    Size index{1U};
    bool physical{false};
    bool requirePwd{false};
    while (index < args.Count)
    {
        const Text option = args.At(index);
        if (Is(option, "--"))
        {
            ++index;
            break;
        }
        if (option.Length < 2U || option.Data[0U] != '-')
        {
            break;
        }
        for (Size j{1U}; j < option.Length; ++j)
        {
            if (option.Data[j] == 'P')
            {
                physical = true;
            }
            else if (option.Data[j] == 'L')
            {
                physical = false;
            }
            else if (option.Data[j] == 'e')
            {
                requirePwd = true;
            }
            else
            {
                return 2;
            }
        }
        ++index;
    }
    if (args.Count > index + 1U)
    {
        return 2;
    }
    bool present{false};
    Text target = index < args.Count ? args.At(index) : Get(Literal("HOME"), present);
    if (index == args.Count && !present)
    {
        return 1;
    }
    bool print = Is(target, "-");
    if (print)
    {
        target = Get(Literal("OLDPWD"), present);
        if (!present)
        {
            return 1;
        }
    }
    if (target.Length == 0U)
    {
        static_cast<void>(Write(Literal("cd: empty directory\n"), 2U));
        return 1;
    }
    if (!DirectoryOperand(target, print, frame))
    {
        return 1;
    }
    Text old = Get(Literal("PWD"), present);
    if (!present || old.Length == 0U || old.Data[0U] != '/')
    {
        Buffer cwd{frame.TextBytes, InputLimit};
        if (m_filesystem.WorkingDirectory == nullptr || !m_filesystem.WorkingDirectory(m_filesystem.Context, cwd))
        {
            return 1;
        }
        old = cwd.View();
    }
    if (!frame.Targets.Add(old))
    {
        return 1;
    }
    target = frame.Targets.At(0U);
    Buffer path{frame.TextBytes, InputLimit};
    if (!physical && target.Data[0U] != '/')
    {
        if (!path.Append(frame.Targets.At(1U)) || !path.Put('/'))
        {
            return 1;
        }
    }
    if (!path.Append(target) || (!physical && !NormalizeDirectory(path)) || !frame.Targets.Add(path.View()))
    {
        return 1;
    }
    if (m_filesystem.ChangeDirectory == nullptr ||
        !m_filesystem.ChangeDirectory(m_filesystem.Context, frame.Targets.At(2U)))
    {
        return 1;
    }
    const Result oldSet = Set(Literal("OLDPWD"), frame.Targets.At(1U), true);
    if (!oldSet.Ok())
    {
        static_cast<void>(Fail(oldSet.Code));
        return 1;
    }
    Text newPwd = frame.Targets.At(2U);
    if (physical)
    {
        path.Length = 0U;
        if (m_filesystem.WorkingDirectory == nullptr || !m_filesystem.WorkingDirectory(m_filesystem.Context, path))
        {
            static_cast<void>(Unset(Literal("PWD")));
            return requirePwd ? 1 : 0;
        }
        newPwd = path.View();
    }
    const Result newSet = Set(Literal("PWD"), newPwd, true);
    if (!newSet.Ok())
    {
        static_cast<void>(Fail(newSet.Code));
        return 1;
    }
    return print ? (Write(newPwd) && Write(Literal("\n")) ? 0 : 1) : 0;
}
int Engine::ExportBuiltin(const Fields& args, ExecutionScratch& frame) noexcept
{
    static_cast<void>(frame);
    Size first{1U};
    if (args.Count == 1U || (args.Count == 2U && Is(args.At(1U), "-p")))
    {
        for (const Variable& v : Environment.Variables)
        {
            if (!v.Present || !v.Exported)
            {
                continue;
            }
            if (!Write(Literal("export ")) || !Write({v.Name, v.NameLength}))
            {
                return 1;
            }
            if (v.NoValue)
            {
                if (!Write(Literal("\n")))
                {
                    return 1;
                }
                continue;
            }
            if (!Write(Literal("='")))
            {
                return 1;
            }
            for (Size i{0U}; i < v.ValueLength; ++i)
            {
                if (!Write(v.Value[i] == '\'' ? Literal("'\\''") : Text{v.Value + i, 1U}))
                {
                    return 1;
                }
            }
            if (!Write(Literal("'\n")))
            {
                return 1;
            }
        }
        return 0;
    }
    if (Is(args.At(first), "--"))
    {
        ++first;
    }
    for (Size i = first; i < args.Count; ++i)
    {
        const Text operand = args.At(i);
        Size equal{0U};
        while (equal < operand.Length && operand.Data[equal] != '=')
        {
            ++equal;
        }
        const Text variable = Slice(operand, 0U, equal);
        bool present{false};
        const Text value =
            equal < operand.Length ? Slice(operand, equal + 1U, operand.Length - equal - 1U) : Get(variable, present);
        const bool unassigned = equal == operand.Length && !present;
        const Result set = Set(variable, value, true);
        if (!set.Ok())
        {
            return 2;
        }
        if (unassigned)
        {
            Environment.Variables[Find(variable)].NoValue = true;
        }
    }
    return 0;
}
int Engine::UnsetBuiltin(const Fields& args, ExecutionScratch& frame) noexcept
{
    static_cast<void>(frame);
    Size first{1U};
    bool functions{false};
    if (first < args.Count && (Is(args.At(first), "-f") || Is(args.At(first), "-v")))
    {
        functions = Is(args.At(first++), "-f");
    }
    if (first < args.Count && Is(args.At(first), "--"))
    {
        ++first;
    }
    for (Size i = first; i < args.Count; ++i)
    {
        if (!Name(args.At(i)))
        {
            return 2;
        }
        if (functions)
        {
            for (Function& function : m_functions)
            {
                if (function.Present && Equal(args.At(i), {function.Name, function.NameLength}))
                {
                    function.Present = false;
                }
            }
        }
        else if (!Unset(args.At(i)).Ok())
        {
            return 2;
        }
    }
    return 0;
}
int Engine::PrintOptions(const bool reusable) noexcept
{
    struct Option
    {
        const char* Name;
        bool State::* Flag;
    };
    static constexpr Option options[]{{"allexport", &State::AllExport}, {"errexit", &State::Errexit},
                                      {"noglob", &State::NoGlob},       {"noexec", &State::NoExecute},
                                      {"nounset", &State::Nounset},     {"noclobber", &State::NoClobber},
                                      {"xtrace", &State::Trace},        {"pipefail", &State::Pipefail}};
    // XCU 2.15 set OPTIONS: +o is suitable for restoring the option state.
    for (const Option& option : options)
    {
        const bool enabled = Environment.*(option.Flag);
        if (reusable && !Write(enabled ? Literal("set -o ") : Literal("set +o ")))
        {
            return 1;
        }
        if (!Write(Literal(option.Name)))
        {
            return 1;
        }
        if (!reusable && !Write(enabled ? Literal(" on") : Literal(" off")))
        {
            return 1;
        }
        if (!Write(Literal("\n")))
        {
            return 1;
        }
    }
    return 0;
}
int Engine::SetBuiltin(const Fields& args, ExecutionScratch& frame) noexcept
{
    static_cast<void>(frame);
    if (args.Count == 1U)
    {
        const Variable* previous{nullptr};
        for (Size emitted{0U}; emitted < VariableLimit; ++emitted)
        {
            const Variable* next{nullptr};
            for (const Variable& candidate : Environment.Variables)
            {
                if (!candidate.Present || candidate.NoValue ||
                    (previous != nullptr &&
                     Compare({candidate.Name, candidate.NameLength}, {previous->Name, previous->NameLength}) <= 0))
                {
                    continue;
                }
                if (next == nullptr ||
                    Compare({candidate.Name, candidate.NameLength}, {next->Name, next->NameLength}) < 0)
                {
                    next = &candidate;
                }
            }
            if (next == nullptr)
            {
                break;
            }
            if (!Write({next->Name, next->NameLength}) || !Write(Literal("='")))
            {
                return 1;
            }
            for (Size i{0U}; i < next->ValueLength; ++i)
            {
                if (!Write(next->Value[i] == '\'' ? Literal("'\\''") : Text{next->Value + i, 1U}))
                {
                    return 1;
                }
            }
            if (!Write(Literal("'\n")))
            {
                return 1;
            }
            previous = next;
        }
        return 0;
    }
    Size first{1U};
    bool operands{false};
    while (first < args.Count)
    {
        const Text option = args.At(first);
        if (Is(option, "--"))
        {
            ++first;
            operands = true;
            break;
        }
        if (option.Length < 2U || (option.Data[0U] != '-' && option.Data[0U] != '+'))
        {
            operands = true;
            break;
        }
        const bool enable = option.Data[0U] == '-';
        if (Is(option, "-o") || Is(option, "+o"))
        {
            if (++first == args.Count)
            {
                return PrintOptions(!enable);
            }
            if (Is(args.At(first), "nounset"))
            {
                Environment.Nounset = enable;
            }
            else if (Is(args.At(first), "noglob"))
            {
                Environment.NoGlob = enable;
            }
            else if (Is(args.At(first), "errexit"))
            {
                Environment.Errexit = enable;
            }
            else if (Is(args.At(first), "noexec"))
            {
                Environment.NoExecute = enable;
            }
            else if (Is(args.At(first), "allexport"))
            {
                Environment.AllExport = enable;
            }
            else if (Is(args.At(first), "noclobber"))
            {
                Environment.NoClobber = enable;
            }
            else if (Is(args.At(first), "xtrace"))
            {
                Environment.Trace = enable;
            }
            else if (Is(args.At(first), "pipefail"))
            {
                Environment.Pipefail = enable;
            }
            else
            {
                return 2;
            }
        }
        else
        {
            for (Size j{1U}; j < option.Length; ++j)
            {
                if (option.Data[j] == 'u')
                {
                    Environment.Nounset = enable;
                }
                else if (option.Data[j] == 'f')
                {
                    Environment.NoGlob = enable;
                }
                else if (option.Data[j] == 'e')
                {
                    Environment.Errexit = enable;
                }
                else if (option.Data[j] == 'n')
                {
                    Environment.NoExecute = enable;
                }
                else if (option.Data[j] == 'a')
                {
                    Environment.AllExport = enable;
                }
                else if (option.Data[j] == 'C')
                {
                    Environment.NoClobber = enable;
                }
                else if (option.Data[j] == 'x')
                {
                    Environment.Trace = enable;
                }
                else
                {
                    return 2;
                }
            }
        }
        ++first;
    }
    if (operands)
    {
        Environment.ParameterCount = args.Count - first;
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            const Text value = args.At(first + i);
            if (value.Length > ValueLimit)
            {
                static_cast<void>(Fail(Error::ValueCapacity));
                return 2;
            }
            for (Size j{0U}; j < value.Length; ++j)
            {
                Environment.Parameters[i].Bytes[j] = value.Data[j];
            }
            Environment.Parameters[i].Length = static_cast<UInt16>(value.Length);
        }
    }
    return 0;
}
int Engine::SourceBuiltin(const Fields& args, ExecutionScratch& frame) noexcept
{
    if (args.Count < 2U || m_filesystem.ReadFile == nullptr)
    {
        return 2;
    }
    Text path = args.At(1U);
    bool slash{false};
    for (Size i{0U}; i < path.Length; ++i)
    {
        slash = slash || path.Data[i] == '/';
    }
    frame.Targets.Count = 0U;
    frame.Targets.Used = 0U;
    if (!slash)
    {
        Buffer resolved{frame.TextBytes, InputLimit};
        if (m_host.Resolve == nullptr || !m_host.Resolve(m_host.Context, path, Environment, resolved) ||
            !frame.Targets.Add(resolved.View()))
        {
            return 1;
        }
        path = frame.Targets.At(0U);
    }
    Buffer source{frame.TextBytes, InputLimit};
    if (!m_filesystem.ReadFile(m_filesystem.Context, path, source))
    {
        return 1;
    }
    frame.ParameterCount = Environment.ParameterCount;
    if (args.Count > 2U)
    {
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            frame.Parameters[i] = Environment.Parameters[i];
        }
        Environment.ParameterCount = args.Count - 2U;
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            const Text value = args.At(i + 2U);
            if (value.Length > ValueLimit)
            {
                return 2;
            }
            for (Size j{0U}; j < value.Length; ++j)
            {
                Environment.Parameters[i].Bytes[j] = value.Data[j];
            }
            Environment.Parameters[i].Length = static_cast<UInt16>(value.Length);
        }
    }
    ++m_sourceDepth;
    const int status = Nested(source.View());
    --m_sourceDepth;
    m_return = false;
    if (args.Count > 2U)
    {
        Environment.ParameterCount = frame.ParameterCount;
        for (Size i{0U}; i < Environment.ParameterCount; ++i)
        {
            Environment.Parameters[i] = frame.Parameters[i];
        }
    }
    return status;
}
int Engine::TypeBuiltin(const Fields& args, ExecutionScratch& frame) noexcept
{
    int status{0};
    for (Size i{1U}; i < args.Count; ++i)
    {
        const Text command = args.At(i);
        bool function{false};
        for (const Function& f : m_functions)
        {
            function = function || (f.Present && Equal(command, {f.Name, f.NameLength}));
        }
        if (SpecialBuiltin(command) || (!function && BuiltinName(command)))
        {
            if (!Write(command) || !Write(Literal(" is a shell builtin\n")))
            {
                return 1;
            }
        }
        else if (function)
        {
            if (!Write(command) || !Write(Literal(" is a shell function\n")))
            {
                return 1;
            }
        }
        else
        {
            Buffer resolved{frame.TextBytes, InputLimit};
            if (m_host.Resolve == nullptr || !m_host.Resolve(m_host.Context, command, Environment, resolved))
            {
                status = 1;
                continue;
            }
            if (!Write(command) || !Write(Literal(" is ")) || !Write(resolved.View()) || !Write(Literal("\n")))
            {
                return 1;
            }
        }
    }
    return status;
}
int Engine::ReadBuiltin(const Fields& args, ExecutionScratch& frame) noexcept
{
    Size first{1U};
    const bool raw = first < args.Count && Is(args.At(first), "-r");
    if (raw)
    {
        ++first;
    }
    if (first < args.Count && Is(args.At(first), "--"))
    {
        ++first;
    }
    if (first == args.Count || m_host.Read == nullptr)
    {
        return 2;
    }
    for (Size i = first; i < args.Count; ++i)
    {
        if (!Name(args.At(i)))
        {
            return 2;
        }
    }
    Buffer input{frame.TextBytes, InputLimit};
    bool eof{false};
    for (Size attempts{0U}; attempts < InputLimit && Step(); ++attempts)
    {
        const Size before = input.Length;
        if (!m_host.Read(m_host.Context, 0U, input, eof) || input.Length > input.Capacity)
        {
            return 1;
        }
        if (raw || eof || input.Length == before || input.Length == 0U || input.Data[input.Length - 1U] != '\n')
        {
            break;
        }
        Size slashes{0U};
        for (Size j = input.Length - 1U; j != 0U && input.Data[j - 1U] == '\\'; --j)
        {
            ++slashes;
        }
        if (slashes % 2U == 0U)
        {
            break;
        }
        input.Length -= 2U;
    }
    if (input.Length != 0U && input.Data[input.Length - 1U] == '\n')
    {
        --input.Length;
    }
    const Result valid = Validate(input.View());
    if (!valid.Ok())
    {
        static_cast<void>(Fail(valid.Code));
        return 1;
    }
    if (m_expansionDepth >= ScriptLimit)
    {
        static_cast<void>(Fail(Error::Depth));
        return 2;
    }
    ExpansionScratch& data = m_expansions[m_expansionDepth++];
    data.Length = 0U;
    for (Size i{0U}; i < input.Length; ++i)
    {
        const bool escape = !raw && input.Data[i] == '\\' && i + 1U < input.Length;
        if (escape)
        {
            ++i;
        }
        if (!Append(data, Slice(input.View(), i, 1U), escape ? Quoted : Splittable))
        {
            --m_expansionDepth;
            return 2;
        }
    }
    bool present{false};
    Text ifs = Get(Literal("IFS"), present);
    if (!present)
    {
        ifs = Literal(" \t\n");
    }
    Size begin{0U};
    Size end = data.Length;
    auto delimiter = [&](Size pos) noexcept -> Size
    {
        return (data.Flags[pos] & Quoted) != 0U ? 0U : IfsWidth({data.Bytes, end}, pos, ifs);
    };
    while (begin < end && Space(data.Bytes[begin]) && delimiter(begin))
    {
        ++begin;
    }
    while (end > begin && Space(data.Bytes[end - 1U]) && delimiter(end - 1U))
    {
        --end;
    }
    for (Size variable = first; variable < args.Count; ++variable)
    {
        Size finish = begin;
        if (variable + 1U == args.Count)
        {
            finish = end;
        }
        else
        {
            while (finish < end && delimiter(finish) == 0U)
            {
                finish += ScalarBytes({data.Bytes, end}, finish);
            }
        }
        const Result set = Set(args.At(variable), {data.Bytes + begin, finish - begin});
        if (!set.Ok())
        {
            --m_expansionDepth;
            static_cast<void>(Fail(set.Code));
            return 2;
        }
        begin = finish;
        while (begin < end && Space(data.Bytes[begin]) && delimiter(begin))
        {
            ++begin;
        }
        if (begin < end && delimiter(begin) != 0U)
        {
            begin += delimiter(begin);
        }
        while (begin < end && Space(data.Bytes[begin]) && delimiter(begin))
        {
            ++begin;
        }
    }
    --m_expansionDepth;
    return eof ? 1 : 0;
}
int Engine::Builtin(const Fields& args, ExecutionScratch& frame, bool& found) noexcept
{
    const Text name = args.At(0U);
    found = true;
    if (Is(name, ":") || Is(name, "true"))
    {
        return 0;
    }
    if (Is(name, "false"))
    {
        return 1;
    }
    if (Is(name, "echo"))
    {
        for (Size i{1U}; i < args.Count; ++i)
        {
            if ((i > 1U && !Write(Literal(" "))) || !Write(args.At(i)))
            {
                return 1;
            }
        }
        return Write(Literal("\n")) ? 0 : 1;
    }
    if (Is(name, "printf"))
    {
        return Printf(args, frame);
    }
    if (Is(name, "test") || Is(name, "["))
    {
        Size end = args.Count;
        if (Is(name, "["))
        {
            if (end < 2U || !Is(args.At(end - 1U), "]"))
            {
                return 2;
            }
            --end;
        }
        TestExpression expression{args, end, m_filesystem, m_host};
        return expression.Run();
    }
    if (Is(name, "pwd"))
    {
        return PrintWorkingDirectory(args, frame);
    }
    if (Is(name, "cd"))
    {
        return ChangeDirectory(args, frame);
    }
    if (Is(name, "export"))
    {
        return ExportBuiltin(args, frame);
    }
    if (Is(name, "unset"))
    {
        return UnsetBuiltin(args, frame);
    }
    if (Is(name, "set"))
    {
        return SetBuiltin(args, frame);
    }
    if (Is(name, "shift"))
    {
        UInt64 count{1U};
        if (args.Count > 2U || (args.Count == 2U && !Decimal(args.At(1U), count)) || count > Environment.ParameterCount)
        {
            return 2;
        }
        for (Size i{0U}; i + count < Environment.ParameterCount; ++i)
        {
            Environment.Parameters[i] = Environment.Parameters[i + count];
        }
        Environment.ParameterCount -= static_cast<Size>(count);
        return 0;
    }
    if (Is(name, "exit") || Is(name, "return"))
    {
        Int64 status = Environment.LastStatus;
        if (args.Count > 2U || (args.Count == 2U && !Signed(args.At(1U), status)))
        {
            return 2;
        }
        if (Is(name, "exit"))
        {
            Environment.ExitRequested = true;
        }
        else
        {
            if (m_functionDepth == 0U && m_sourceDepth == 0U)
            {
                return 2;
            }
            m_return = true;
        }
        return static_cast<int>(static_cast<UInt64>(status) & 255U);
    }
    if (Is(name, "eval"))
    {
        Buffer source{frame.TextBytes, InputLimit};
        for (Size i{1U}; i < args.Count; ++i)
        {
            if ((i > 1U && !source.Put(' ')) || !source.Append(args.At(i)))
            {
                static_cast<void>(Fail(Error::InputTooLarge));
                return 2;
            }
        }
        return Nested(source.View());
    }
    if (Is(name, "."))
    {
        return SourceBuiltin(args, frame);
    }
    if (Is(name, "type"))
    {
        return TypeBuiltin(args, frame);
    }
    if (Is(name, "read"))
    {
        return ReadBuiltin(args, frame);
    }
    found = false;
    return 0;
}
} // Namespace Asteria::User::Shell
