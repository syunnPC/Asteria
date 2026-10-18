// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/RootService.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "Abi/PagerBootstrap.hpp"
#include "User/Loader/ExecutableFormats.hpp"
#include "User/Pager/ComponentImage.hpp"
#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
namespace
{
// The images of the incarnations' program, of the Directory and of the
// client in the kernel-mapped module table.
constexpr const char* ComponentImageName{Abi::PagerDriverModuleName};
constexpr char DirectoryImageName[] = "directory";
constexpr char ClientImageName[] = "client";
constexpr char UsbHostImageName[] = "usb";
// The display broker's tag, apart from the scenarios' 1, 2,...: its epoch.

// How long the root service lets the broker block again before it kills it:
// longer than a user time slice.
constexpr Foundation::UInt64 DisplayKillPauseMicroseconds{100'000U};

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

[[nodiscard]] constexpr PagerServeOutcome MakeOutcome(const PagerServeError error,
                                                      const Abi::SystemCallError systemCall) noexcept
{
    return PagerServeOutcome{.Error = error, .SystemCall = systemCall, .Resolved = 0U};
}

// Whether the table has the image name, described under rules into image;
// false for one it has but cannot describe, through valid.
[[nodiscard]] bool FindImage(const PagerThreadLaunch& launch, const char* const name, LoadedImage& image,
                             bool& valid) noexcept
{
    if (!FindComponentImage(launch.ComponentImages, launch.ComponentImagesBytes, name, image.Bytes, image.Size))
    {
        valid = true;
        return false;
    }
    valid = Loader::DescribeExecutable(image.Bytes, image.Size, launch.Component.Image, image.Layout) ==
            Loader::LoaderError::None;
    return true;
}

} // namespace

void RootService::Configure(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                            PagerProgram& pager, const PagerThreadLaunch& launch) noexcept
{
    m_bootstrap = &bootstrap;
    m_launch = &launch;
    m_calls.Configure(client);
    m_devices.Configure(client, bootstrap.Devices);
    m_loader.Configure(m_calls, bootstrap, launch, m_devices);
    m_supervisor.Configure(m_calls, m_loader, m_devices, pager, bootstrap, launch.Component);
    m_context.Client = &client;
    m_context.Bootstrap = &bootstrap;
    m_context.Management = bootstrap.Management;
    m_context.Diagnostic = bootstrap.Diagnostic;
    m_context.Display = &m_display;
    m_broker = nullptr;
    m_display = DisplayMirror{};
    m_context.Launch = &launch;
    m_context.Calls = &m_calls;
    m_context.Supervisor = &m_supervisor;
    m_context.Devices = &m_devices;
    m_context.Report = {};
    m_context.Reply = {};
    m_context.DirectoryEndpoint = {};
    m_context.OwnSpace = bootstrap.OwnSpace;
    m_context.WindowStart = bootstrap.WindowStart;
    m_context.DriverService = {};
    m_context.DriverGeneration = 0U;
    m_context.ServiceFlags = 0U;
    m_context.NextTag = 0U;
}

bool RootService::FindImages() noexcept
{
    bool valid{false};
    if (!FindImage(*m_launch, ComponentImageName, m_context.Component, valid) || !valid)
    {
        return false;
    }
    m_context.Directory = LoadedImage{};
    m_context.ClientImage = LoadedImage{};
    static_cast<void>(FindImage(*m_launch, DirectoryImageName, m_context.Directory, valid));
    if (!valid)
    {
        return false;
    }
    static_cast<void>(FindImage(*m_launch, ClientImageName, m_context.ClientImage, valid));
    if (!valid)
    {
        return false;
    }
    m_context.UsbHost = LoadedImage{};
    static_cast<void>(FindImage(*m_launch, UsbHostImageName, m_context.UsbHost, valid));
    if (!valid)
    {
        return false;
    }
    return FindImage(*m_launch, "display", m_context.DisplayImage, valid) && valid;
}

PagerServeError RootService::StartDisplay(Abi::SystemCallError& systemCall) noexcept
{
    const PagerScanout& scanout = m_bootstrap->Scanout;
    constexpr Foundation::UInt64 Needed{Abi::PagerManagementResourceDomains | Abi::PagerManagementWaitSets};
    if ((scanout.Region.Selector == 0U) || ((m_bootstrap->ManagementFeatures & Needed) != Needed))
    {
        return PagerServeError::None;
    }
    m_broker =
        StartComponent(m_context, m_context.DisplayImage, m_displayTag, Abi::ComponentBehaviorDisplay, systemCall);
    Component::DisplayInfo info{};
    if ((m_broker == nullptr) || !QueryDisplay(info, systemCall))
    {
        return PagerServeError::ManagementFailed;
    }
    if (!m_supervisor.WatchService(*m_broker, systemCall, RecoverWatchdogDisplay, this))
    {
        return PagerServeError::ManagementFailed;
    }
    m_display.Console = m_broker->Service;
    m_display.Broker = m_broker;
    // "# display: 1280x800 bgrx, console 160x50"
    char line[80]{};
    Foundation::Size length{0U};
    AppendText(line, length, "# display: ");
    AppendDecimal(line, length, info.Width);
    AppendText(line, length, "x");
    AppendDecimal(line, length, info.Height);
    AppendText(line, length, (scanout.Format == 1U) ? " rgbx, console " : " bgrx, console ");
    AppendDecimal(line, length, info.Columns);
    AppendText(line, length, "x");
    AppendDecimal(line, length, info.Rows);
    AppendText(line, length, "\n");
    WriteLine(m_context, line, length);
    WriteDisplayMetrics(m_context, info);
    if (m_launch->Component.Clock != nullptr)
    {
        m_display.LastMetricsCounter = m_launch->Component.Clock();
    }
    Foundation::UInt64 epoch{0U};
    Foundation::UInt64 sequence{0U};
    return FencePointer(m_context, epoch, sequence, systemCall) ? PagerServeError::None
                                                                : PagerServeError::ManagementFailed;
}

bool RootService::QueryDisplay(Component::DisplayInfo& info, Abi::SystemCallError& systemCall) noexcept
{
    const Foundation::UInt64 transaction = ++m_display.Transaction;
    alignas(8) Foundation::UInt8 frame[Component::DisplayQueryBytes]{};
    const Foundation::Size size = Component::WriteDisplayQuery(frame, transaction, m_context.Report);
    if (!Check(m_context.Client->Send(m_broker->Service, AddressOf(frame), size), systemCall))
    {
        return false;
    }
    // The answer follows drawing every line written before it, and the first
    // follows clearing the screen: through an uncached scanout on real
    // hardware that takes seconds, so the wait is a minute while the broker
    // runs, not the two seconds of an ordinary answer.
    constexpr Foundation::UInt64 DisplayAnswerAttempts{6'000U};
    const Foundation::UInt64 received = m_context.Supervisor->AwaitMessageFor(
        *m_broker, m_context.Report, m_context.Delivery, sizeof(m_context.Delivery), DisplayAnswerAttempts, systemCall);
    Component::DisplayMessage& message = m_context.DecodedDisplayReply;
    if ((received == 0U) ||
        !Component::ReadDisplayFrame(m_context.Delivery, static_cast<Foundation::Size>(received), message) ||
        (message.Method != Component::DisplayReplyMethod) || (message.Transaction != transaction) ||
        (message.Words[1U] != Component::DisplayDone))
    {
        return false;
    }
    info = Component::UnpackDisplayInfo(message);
    return true;
}

// The fence comes first, so the screen holds every line before the kill; a
// line the broker's queue refused (the mirroring stopped) fails nothing here:
// the runner's comparison of the screen does.
PagerServeError RootService::StopDisplay(Abi::SystemCallError& systemCall) noexcept
{
    if (m_broker == nullptr)
    {
        return PagerServeError::None;
    }
    Foundation::UInt64 epoch{0U};
    Foundation::UInt64 sequence{0U};
    const bool fenced = FencePointer(m_context, epoch, sequence, systemCall);
    m_display.Console = {};
    if (!fenced)
    {
        return PagerServeError::ManagementFailed;
    }
    ComponentRecord& broker = *m_broker;
    m_broker = nullptr;
    // The broker blocks again once it answered: a kill then aborts its wait.
    const PagerServeError paused = m_supervisor.Pause(DisplayKillPauseMicroseconds, systemCall);
    const PagerServeError stopped = (paused != PagerServeError::None) ? paused : m_supervisor.Stop(broker, systemCall);
    if (stopped != PagerServeError::None)
    {
        return stopped;
    }
    const bool killed = broker.ExitCode == Abi::KilledExitCode;
    if (!killed)
    {
        ReportUnexpectedEnd(m_context, broker.Tag, broker.ExitCode, false);
    }
    if (!m_supervisor.Teardown(broker, systemCall) || !killed)
    {
        return PagerServeError::ManagementFailed;
    }
    constexpr char stoppedLine[]{"# display: stopped\n"};
    WriteLine(m_context, stoppedLine, sizeof(stoppedLine) - 1U);
    return PagerServeError::None;
}

bool RootService::RecoverWatchdogDisplay(void* const argument, ComponentRecord& broker,
                                         Abi::SystemCallError& error) noexcept
{
    auto& root = *static_cast<RootService*>(argument);
    root.m_display.Console = {};
    if (root.m_supervisor.Stop(broker, error) != PagerServeError::None || !root.m_supervisor.Teardown(broker, error))
    {
        return false;
    }
    root.m_broker = nullptr;
    ++root.m_displayTag;
    return root.StartDisplay(error) == PagerServeError::None;
}

bool RootService::Ran(const char* const scenario, const PagerServeError served) noexcept
{
    if (served == PagerServeError::None)
    {
        return true;
    }
    m_failedScenario = scenario;
    m_failedError = served;
    return false;
}

// Stop and tear down the failed scenario's components, skip later scenarios,
// and continue serving pager faults.
PagerServeOutcome RootService::Abandon(const Abi::SystemCallError systemCall) noexcept
{
    // The broker dies with the rest; the failure's line reaches the debug
    // channel only.
    m_display.Console = {};
    m_broker = nullptr;
    constexpr char Prefix[] = "# root service: ";
    constexpr char TornDown[] = " failed, components torn down\n";
    constexpr char Standing[] = " failed, components left standing\n";
    Abi::SystemCallError abandoned{Abi::SystemCallError::None};
    const bool clean = m_supervisor.Abandon(abandoned);
    const char* const parts[]{Prefix, m_failedScenario, clean ? TornDown : Standing};
    char line[80]{};
    Foundation::Size length{0U};
    for (const char* text : parts)
    {
        for (; (*text != '\0') && (length < (sizeof(line) - 1U)); ++text)
        {
            line[length++] = *text;
        }
    }
    WriteLine(m_context, line, length);
    return MakeOutcome(m_failedError, systemCall);
}

} // Namespace Asteria::User::Pager
