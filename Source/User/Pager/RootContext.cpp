// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/RootContext.hpp"

#include "Abi/Clock.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/IpcCanonicalFrame.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// Appends value in decimal to line at length.
void AppendDecimal(char* const line, Foundation::Size& length, const Foundation::UInt64 value) noexcept
{
    char digits[20]{};
    Foundation::Size count{0U};
    Foundation::UInt64 left = value;
    do
    {
        digits[count++] = static_cast<char>('0' + (left % 10U));
        left /= 10U;
    } while (left != 0U);
    while (count != 0U)
    {
        line[length++] = digits[--count];
    }
}

void AppendText(char* const line, Foundation::Size& length, const char* text) noexcept
{
    for (; *text != '\0'; ++text)
    {
        line[length++] = *text;
    }
}

// How long the root service lets the display broker drain its queue before it sends a refused line
// again, and how often.
constexpr Foundation::UInt64 MirrorPauseMicroseconds{10'000U};
// A minute: uncached hardware scanouts may redraw for seconds while the queue is full.
constexpr Foundation::UInt64 MirrorAttempts{6'000U};

[[nodiscard]] ComponentRecord* Build(ScenarioContext& context, const LoadedImage& image, const Foundation::UInt64 tag,
                                     const Foundation::UInt64 behavior, const ForeignImage& foreign,
                                     const bool deferred, Abi::SystemCallError& systemCall) noexcept
{
    ComponentRecord* const component = context.Supervisor->Allocate();
    const ComponentBuild build{.Image = &image,
                               .Tag = tag,
                               .Behavior = behavior,
                               .Foreign = foreign,
                               .Report = context.Report,
                               .Directory = context.DirectoryEndpoint,
                               .DriverService = context.DriverService,
                               .DriverGeneration = context.DriverGeneration,
                               .ServiceFlags = context.ServiceFlags,
                               .Deferred = deferred,
                               .SessionInput = context.SessionInput,
                               .SessionInputEpoch = context.SessionInputEpoch,
                               .IdentityLimiter = context.IdentityLimiter};
    return ((component != nullptr) && context.Supervisor->Start(*component, build, systemCall)) ? component : nullptr;
}
} // Namespace

ComponentRecord* StartComponent(ScenarioContext& context, const LoadedImage& image, const Foundation::UInt64 tag,
                                const Foundation::UInt64 behavior, const ForeignImage& foreign,
                                Abi::SystemCallError& systemCall) noexcept
{
    return Build(context, image, tag, behavior, foreign, false, systemCall);
}

ComponentRecord* BuildComponent(ScenarioContext& context, const LoadedImage& image, const Foundation::UInt64 tag,
                                const Foundation::UInt64 behavior, Abi::SystemCallError& systemCall) noexcept
{
    return Build(context, image, tag, behavior, ForeignImage{}, true, systemCall);
}

ComponentRecord* StartComponent(ScenarioContext& context, const LoadedImage& image, const Foundation::UInt64 tag,
                                const Foundation::UInt64 behavior, Abi::SystemCallError& systemCall) noexcept
{
    return StartComponent(context, image, tag, behavior, ForeignImage{}, systemCall);
}

Foundation::UInt64 AwaitMessage(ScenarioContext& context, const ComponentRecord& component,
                                const System::CapabilityHandle endpoint, Abi::SystemCallError& systemCall) noexcept
{
    return context.Supervisor->AwaitMessage(component, endpoint, context.Delivery, sizeof(context.Delivery),
                                            systemCall);
}

void WriteLine(const ScenarioContext& context, const char* const line, const Foundation::Size bytes) noexcept
{
    static_cast<void>(context.Client->DebugWrite(context.Diagnostic, AddressOf(line), bytes));
    WriteConsole(context, line, bytes);
}

void WriteConsole(const ScenarioContext& context, const char* const line, const Foundation::Size bytes) noexcept
{
    if (context.ConsoleObserver != nullptr)
    {
        context.ConsoleObserver(context.ConsoleObserverContext, line, bytes);
    }
    DisplayMirror* const mirror = context.Display;
    const Foundation::UInt8* const text = reinterpret_cast<const Foundation::UInt8*>(line);
    for (Foundation::Size offset{0U}; (mirror != nullptr) && (mirror->Console.Selector != 0U) && (offset < bytes);)
    {
        const Foundation::Size left = bytes - offset;
        const Foundation::Size count =
            (left < Component::DisplayMaximumWriteBytes) ? left : Component::DisplayMaximumWriteBytes;
        alignas(8) Foundation::UInt8 frame[Component::DisplayConsoleWriteBytes]{};
        const Foundation::Size size =
            Component::WriteDisplayConsoleWrite(frame, ++mirror->Transaction, text + offset, count);
        Abi::SystemCallError sent{Abi::SystemCallError::WouldBlock};
        for (Foundation::UInt64 attempt{0U}; (attempt < MirrorAttempts) && (sent == Abi::SystemCallError::WouldBlock);
             ++attempt)
        {
            sent = context.Client->Send(mirror->Console, AddressOf(frame), size).Error;
            Abi::SystemCallError paused{Abi::SystemCallError::None};
            if ((sent == Abi::SystemCallError::WouldBlock) &&
                (context.Supervisor->Pause(MirrorPauseMicroseconds, paused) != PagerServeError::None))
            {
                break;
            }
        }
        if (sent != Abi::SystemCallError::None)
        {
            mirror->Console = {};
            mirror->Dropped = true;
        }
        offset += count;
    }
}

void WriteBetweenLines(const ScenarioContext& context, const char* const line, const Foundation::Size bytes) noexcept
{
    const DisplayMirror* const mirror = context.Display;
    const Foundation::Size partial =
        (mirror == nullptr) || (mirror->PartialLine == nullptr) ? 0U : mirror->PartialBytes;
    char erase[48U]{};
    for (Foundation::Size index{0U}; index < sizeof(erase); index += 3U)
    {
        erase[index] = '\b';
        erase[index + 1U] = ' ';
        erase[index + 2U] = '\b';
    }
    for (Foundation::Size left{partial}; left != 0U;)
    {
        const Foundation::Size count = left < sizeof(erase) / 3U ? left : sizeof(erase) / 3U;
        WriteConsole(context, erase, count * 3U);
        left -= count;
    }
    WriteLine(context, line, bytes);
    if (partial != 0U)
    {
        WriteConsole(context, mirror->PartialLine, partial);
    }
}

void WriteDisplayMetrics(const ScenarioContext& context, const Component::DisplayInfo& info) noexcept
{
    if ((info.Features & Component::DisplayFeatureMetrics) == 0U)
    {
        return;
    }
    // Seven uint64 decimal fields plus literals fit even at their maxima.
    char line[320U]{};
    Foundation::Size length{0U};
    AppendText(line, length, "# display: scanout ");
    AppendText(line, length, info.ScanoutPolicy == Component::DisplayScanoutWc ? "wc" : "uc");
    AppendText(line, length, ", fill ");
    AppendDecimal(line, length, info.FillBytes);
    AppendText(line, length, " bytes ");
    AppendDecimal(line, length, info.FillMicroseconds);
    AppendText(line, length, " us, scroll ");
    AppendDecimal(line, length, info.LatestPixels);
    AppendText(line, length, " px ");
    AppendDecimal(line, length, info.LatestMicroseconds);
    AppendText(line, length, " us, slowest ");
    AppendDecimal(line, length, info.SlowestPixels);
    AppendText(line, length, " px ");
    AppendDecimal(line, length, info.SlowestMicroseconds);
    if ((info.Features & Component::DisplayFeatureSession) != 0U && info.SessionPixels != 0U)
    {
        AppendText(line, length, " us, session ");
        AppendDecimal(line, length, info.SessionPixels);
        AppendText(line, length, " px ");
        AppendDecimal(line, length, info.SessionMicroseconds);
    }
    AppendText(line, length,
               (info.Features & Component::DisplayFeatureTiming) == 0U ? " us (clock unavailable)\n" : " us\n");
    WriteBetweenLines(context, line, length);
}

// Product input already queries after each delivery. Refresh worsening scroll
// measurements there; scenario fences must leave their observed screen alone.
[[gnu::noinline]] void RefreshDisplayMetrics(const ScenarioContext& context,
                                             const Component::DisplayInfo& info) noexcept
{
    DisplayMirror& mirror = *context.Display;
    if (!context.OptionalInputDrivers || (context.Launch == nullptr) || (context.Bootstrap == nullptr) ||
        (context.Launch->Component.Clock == nullptr) || (context.Bootstrap->ClockTicksPerMicrosecond == 0U) ||
        ((info.Features & Component::DisplayFeatureTiming) == 0U) || (info.SlowestPixels == 0U) ||
        (info.SlowestMicroseconds <= mirror.ReportedSlowestMicroseconds) ||
        (info.SlowestMicroseconds - mirror.ReportedSlowestMicroseconds <= mirror.ReportedSlowestMicroseconds / 5U))
    {
        return;
    }
    const Foundation::UInt64 now = context.Launch->Component.Clock();
    if ((Abi::ElapsedClockTicks(now, mirror.LastMetricsCounter)) / context.Bootstrap->ClockTicksPerMicrosecond <
        1'000'000U)
    {
        return;
    }
    mirror.LastMetricsCounter = now;
    mirror.ReportedSlowestMicroseconds = info.SlowestMicroseconds;
    // A user's partial line is redrawn after the metrics line, not split.
    WriteDisplayMetrics(context, info);
}

bool FencePointer(ScenarioContext& context, Foundation::UInt64& epoch, Foundation::UInt64& sequence,
                  Abi::SystemCallError& error) noexcept
{
    DisplayMirror* const mirror = context.Display;
    if ((mirror == nullptr) || (mirror->Broker == nullptr) || (mirror->Console.Selector == 0U))
    {
        epoch = 0U;
        sequence = 0U;
        return true;
    }
    const Foundation::UInt64 transaction = ++mirror->Transaction;
    const System::CapabilityHandle endpoint = context.Reply.Selector == 0U ? context.Report : context.Reply;
    const Foundation::Size bytes = Component::WriteDisplayQuery(context.Frame, transaction, endpoint);
    Abi::SystemCallError sent{Abi::SystemCallError::WouldBlock};
    for (Foundation::Size attempt{0U}; (attempt < 2'000U) && (sent == Abi::SystemCallError::WouldBlock); ++attempt)
    {
        sent = context.Client->Send(mirror->Console, AddressOf(context.Frame), bytes).Error;
        if ((sent == Abi::SystemCallError::WouldBlock) &&
            (context.Supervisor->Pause(10'000U, error) != PagerServeError::None))
        {
            return false;
        }
    }
    if (sent != Abi::SystemCallError::None)
    {
        error = sent;
        return false;
    }
    const Foundation::UInt64 received = context.Supervisor->AwaitMessageFor(*mirror->Broker, endpoint, context.Delivery,
                                                                            sizeof(context.Delivery), 6'000U, error);
    Component::DisplayMessage& message = context.DecodedDisplayReply;
    if ((received == 0U) ||
        !Component::ReadDisplayFrame(context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Method != Component::DisplayReplyMethod) || (message.Transaction != transaction) ||
        (message.Words[1U] != Component::DisplayDone))
    {
        return false;
    }
    const Component::DisplayInfo info = Component::UnpackDisplayInfo(message);
    epoch = info.Epoch;
    sequence = info.PointerSequence;
    RefreshDisplayMetrics(context, info);
    char line[64U]{};
    Foundation::Size length{0U};
    AppendText(line, length, "# pointer: ");
    AppendDecimal(line, length, info.PointerX);
    AppendText(line, length, ",");
    AppendDecimal(line, length, info.PointerY);
    AppendText(line, length, " sequence ");
    AppendDecimal(line, length, info.PointerSequence);
    AppendText(line, length, "\n");
    static_cast<void>(context.Client->DebugWrite(context.Diagnostic, AddressOf(line), length));
    return true;
}

bool SetPointerRoute(ScenarioContext& context, const ComponentRecord& input, Abi::SystemCallError& error) noexcept
{
    Foundation::UInt64 epoch{0U};
    Foundation::UInt64 sequence{0U};
    if (!FencePointer(context, epoch, sequence, error))
    {
        return false;
    }
    if (epoch == 0U)
    {
        return true;
    }
    // The attachment copies only Invoke, even though the root's handle has Manage.
    const Foundation::UInt64 words[4U]{input.Tag, epoch, sequence, 0U};
    const Foundation::UInt64 transaction = ++context.Display->Transaction;
    const Foundation::Size bytes = Component::WriteInputPointerRoute(
        context.Frame, transaction, words, context.Reply.Selector == 0U ? context.Report : context.Reply,
        &context.Display->Console);
    if (!Check(context.Client->Send(input.Service, AddressOf(context.Frame), bytes), error))
    {
        return false;
    }
    const Foundation::UInt64 received =
        AwaitMessage(context, input, context.Reply.Selector == 0U ? context.Report : context.Reply, error);
    Component::InputMessage message{};
    return (received != 0U) &&
           Component::ReadInputFrame(context.Delivery, static_cast<Foundation::Size>(received), message) &&
           (message.Method == Component::InputReplyMethod) && (message.Transaction == transaction) &&
           (message.Words[1U] == 1U);
}

void ReportUnexpectedEnd(const ScenarioContext& context, const Foundation::UInt64 incarnation,
                         const Foundation::UInt64 exitCode, const bool crashed) noexcept
{
    constexpr char Prefix[] = "# component: incarnation ";
    constexpr char Middle[] = " ended with 0x";
    constexpr char Suffix[] = ", crashed ";
    constexpr char Digits[] = "0123456789ABCDEF";
    char line[96]{};
    Foundation::Size length{0U};
    for (const char character : Prefix)
    {
        line[length] = character;
        length += (character != '\0') ? 1U : 0U;
    }
    line[length++] = Digits[incarnation & 0xFU];
    for (const char character : Middle)
    {
        line[length] = character;
        length += (character != '\0') ? 1U : 0U;
    }
    for (Foundation::UInt64 shift{64U}; shift != 0U; shift -= 4U)
    {
        line[length++] = Digits[(exitCode >> (shift - 4U)) & 0xFU];
    }
    for (const char character : Suffix)
    {
        line[length] = character;
        length += (character != '\0') ? 1U : 0U;
    }
    line[length++] = crashed ? '1' : '0';
    line[length++] = '\n';
    WriteLine(context, line, length);
}

} // Namespace Asteria::User::Pager
