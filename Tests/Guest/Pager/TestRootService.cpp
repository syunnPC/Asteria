// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/TestRootService.hpp"

#include "Guest/Pager/Ia64FaultScenarios.hpp"
#include "Guest/Pager/Amd64SmpScenarios.hpp"

#include "User/Pager/DeviceInventory.hpp"
#include "User/Pager/ProductDesktop.hpp"
#include "User/Pager/InputTerminal.hpp"
#include "User/Pager/ComponentImage.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Loader/ExecutableFormats.hpp"

namespace Asteria::User::Pager
{
namespace
{
// The names a failed scenario is reported by.
constexpr char SharedEndpointsName[] = "endpoints";
constexpr char DriverRecoveryName[] = "driver-recovery";
constexpr char BlockName[] = "block";
constexpr char FsName[] = "fs";
constexpr char NetName[] = "net";
constexpr char EchoName[] = "echo";
constexpr char ServicesName[] = "services";
constexpr char LoaderName[] = "loader";
constinit LoadedImage g_probe{};
constexpr char DisplayName[] = "display";
[[nodiscard]] constexpr const char* IncarnationName(const Foundation::UInt64 behavior) noexcept
{
    return (behavior == Abi::ComponentBehaviorCrash)     ? "crash"
           : (behavior == Abi::ComponentBehaviorHang)    ? "hang"
           : (behavior == Abi::ComponentBehaviorReport)  ? "report"
           : (behavior == Abi::ComponentBehaviorAccount) ? "account"
                                                         : "driver";
}
[[nodiscard]] constexpr PagerServeOutcome MakeOutcome(const PagerServeError error,
                                                      const Abi::SystemCallError systemCall) noexcept
{
    return PagerServeOutcome{.Error = error, .SystemCall = systemCall, .Resolved = 0U};
}
} // Namespace

bool TestRootService::RunFaultChannel(Abi::SystemCallError& error) noexcept
{
    auto* const supervisor =
        StartComponent(m_context, m_context.Component, 10U, Abi::ComponentBehaviorFaultSupervisor, error);
    if (supervisor == nullptr)
    {
        return false;
    }
    const auto received = AwaitMessage(m_context, *supervisor, m_context.Report, error);
    if (received != Component::EchoDeliveryHeaderBytes + Component::EchoReplyBytes ||
        Component::GetEchoAttachmentCount(m_context.Delivery) != 1U)
    {
        return false;
    }
    const auto channel = Component::ReadEchoHandle(m_context.Delivery, 0U);
    auto* const child = m_supervisor.Allocate();
    const ComponentBuild build{.Image = &m_context.Component,
                               .Tag = 11U,
                               .Behavior = Abi::ComponentBehaviorCrash,
                               .Foreign = {},
                               .Report = m_context.Report,
                               .Directory = {},
                               .DriverService = {},
                               .DriverGeneration = 0U,
                               .ServiceFlags = 0U,
                               .Deferred = false,
                               .AccountParent = supervisor->Account,
                               .FaultChannel = channel};
    if (child == nullptr || !m_supervisor.Start(*child, build, error) ||
        m_supervisor.Supervise(*supervisor, error) != PagerServeError::None ||
        supervisor->ExitCode != Abi::ComponentFaultSupervisorExitCode ||
        m_supervisor.Supervise(*child, error) != PagerServeError::None || !m_supervisor.Teardown(*child, error) ||
        !m_supervisor.Teardown(*supervisor, error) || !m_calls.Close(channel, error))
    {
        return false;
    }
    static constexpr char line[]{"# fault-channel: created supervisor received identity and resolved token, drained\n"};
    WriteLine(m_context, line, sizeof(line) - 1U);
    return true;
}

PagerServeOutcome TestRootService::Run(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                       PagerProgram& pager, const PagerThreadLaunch& launch) noexcept
{
    Configure(client, bootstrap, pager, launch);
    if (((m_bootstrap->ManagementFeatures & Abi::PagerManagementDomains) == 0U) ||
        ((m_bootstrap->ManagementFeatures & Abi::PagerManagementMapping) == 0U) ||
        (m_launch->ComponentImages == nullptr))
    {
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    Abi::SystemCallError systemCall{Abi::SystemCallError::None};
    if ((m_bootstrap->WindowPages < m_launch->Component.Image.MaximumSegmentPages) ||
        (m_launch->Component.WatchdogMicroseconds == 0U) || !FindImages() ||
        !m_calls.CreateEndpoint(m_bootstrap->Management, m_context.Report, systemCall))
    {
        return MakeOutcome(PagerServeError::ManagementFailed, systemCall);
    }
    if (!Ran("amd64-smp",
             RunAmd64SmpScenarios(m_context, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed))
    {
        return Abandon(systemCall);
    }
    if (!Ran("amd64-return-boundary",
             RunAmd64ReturnBoundary(systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed))
    {
        return Abandon(systemCall);
    }
    if (!Ran("ia64-faults",
             RunIa64FaultScenarios(m_context, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed))
    {
        return Abandon(systemCall);
    }
    if (((m_bootstrap->ManagementFeatures & Abi::PagerManagementFrames) != 0U) &&
        !Ran("frame-rights", RunFrameRights(systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed))
    {
        return Abandon(systemCall);
    }
#if defined(ASTERIA_COMPONENT_WATCHDOG_PROBE) && ASTERIA_COMPONENT_WATCHDOG_PROBE
    m_displayTag = 0x8000'0000'0000'0100ULL;
#endif
    // First the display broker, which runs beside every scenario.
    if (!Ran(DisplayName, StartDisplay(systemCall)))
    {
        return Abandon(systemCall);
    }
#if defined(ASTERIA_COMPONENT_WATCHDOG_PROBE) && ASTERIA_COMPONENT_WATCHDOG_PROBE
    ReadDeviceInventory(m_context);
    for (Foundation::Size attempt{0U}; attempt < 300U && AsteriaComponentWatchdogEvidence[0U].Restarts == 0U; ++attempt)
    {
        if (m_supervisor.Step(100'000U, systemCall) != PagerServeError::None)
        {
            return Abandon(systemCall);
        }
    }
    Component::DisplayInfo watchdogInfo{};
    if (AsteriaComponentWatchdogEvidence[0U].Restarts != 1U || !QueryDisplay(watchdogInfo, systemCall) ||
        m_broker->Tag != 0x8000'0000'0000'0101ULL)
    {
        return Abandon(systemCall);
    }
    constexpr char recovered[]{"# watchdog-test: display restarted and answering\n"};
    WriteLine(m_context, recovered, sizeof(recovered) - 1U);
    if (StopDisplay(systemCall) != PagerServeError::None)
    {
        return Abandon(systemCall);
    }
    WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
    return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
#endif
    ReadDeviceInventory(m_context);
    // Firmware scenario: the product launch hook with a real boot-log window.
    if (FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes,
                           Abi::ComponentToolkitPreviewModule, g_probe.Bytes, g_probe.Size))
    {
        ConfigureProductDesktop(m_context);
        m_context.NextTag = 1U;
        if (!m_calls.CreateEndpoint(m_bootstrap->Management, m_context.Reply, systemCall) ||
            StartInputTerminal(m_context, systemCall) != PagerServeError::None)
        {
            return Abandon(systemCall);
        }
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    if (FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "fault-channel", g_probe.Bytes,
                           g_probe.Size))
    {
        if (!Ran("fault-channel",
                 RunFaultChannel(systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed) ||
            !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    if (FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "session-large", g_probe.Bytes,
                           g_probe.Size))
    {
        if (!Ran("session-large",
                 RunSessionInput(systemCall, true) ? PagerServeError::None : PagerServeError::ManagementFailed) ||
            !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    if (FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "session-input", g_probe.Bytes,
                           g_probe.Size) ||
        FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "session-stress", g_probe.Bytes,
                           g_probe.Size))
    {
        if (!Ran("session-input",
                 RunSessionInput(systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed) ||
            !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    if (FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "session-test", g_probe.Bytes,
                           g_probe.Size))
    {
        if (!Ran("session-test",
                 RunCompositor(systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed) ||
            !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    // Activated only by the VM case's renamed test module receipt.
    LoadedImage& probe = g_probe;
    if (FindComponentImage(m_launch->ComponentImages, m_launch->ComponentImagesBytes, "session-probe", probe.Bytes,
                           probe.Size))
    {
        if (Loader::DescribeExecutable(probe.Bytes, probe.Size, m_launch->Component.Image, probe.Layout) !=
                Loader::LoaderError::None ||
            !Ran("session-probe",
                 RunSessionProbe(probe, systemCall) ? PagerServeError::None : PagerServeError::ManagementFailed) ||
            !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    if (m_devices.IsPresent(Abi::PagerDeviceXhci))
    {
        m_context.NextTag = 1U;
        if (!m_calls.CreateEndpoint(m_bootstrap->Management, m_context.Reply, systemCall) ||
            !Ran("xhci", RunInputDemo(m_context, systemCall)) || !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Reply, Abi::ObjectTypeEndpoint, systemCall) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    // A USB host table with an EHCI (the default machines have none: q35
    // attaches no USB host controller, itanium2-vpc's own are OHCI and UHCI)
    // runs the USB host group's recovery alone, as an xHCI runs its driver's.
    bool ehci{false};
    for (Foundation::Size slot{0U}; slot < Abi::PagerUsbHostControllers; ++slot)
    {
        ehci = ehci || (m_devices.IsPresent(Abi::PagerDeviceUsbHost + slot) && (UsbHostInterface(slot) == 0x20U));
    }
    if (ehci && (m_context.UsbHost.Bytes != nullptr))
    {
        m_context.NextTag = 1U;
        if (!m_calls.CreateEndpoint(m_bootstrap->Management, m_context.Reply, systemCall) ||
            !Ran("usb", RunUsbHostRecoveryDemo(m_context, systemCall)) || !Ran(DisplayName, StopDisplay(systemCall)) ||
            !m_calls.Destroy(m_context.Reply, Abi::ObjectTypeEndpoint, systemCall) ||
            !m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall))
        {
            return Abandon(systemCall);
        }
        WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
        return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
    }
    constexpr Foundation::Size incarnations{sizeof(ComponentIncarnations) / sizeof(ComponentIncarnations[0])};
    for (Foundation::Size index{0U}; index < incarnations; ++index)
    {
        const Foundation::UInt64 behavior = ComponentIncarnations[index];
        if (!Ran(IncarnationName(behavior), RunIncarnation(m_context, index + 1U, behavior, systemCall)))
        {
            return Abandon(systemCall);
        }
    }
    // Where components have domains of their own and a window, one creates in its own.
    Foundation::UInt64 tag{incarnations + 1U};
    if (((m_bootstrap->ManagementFeatures & Abi::PagerManagementResourceDomains) != 0U) &&
        (m_launch->Component.WindowPages != 0U) &&
        !Ran(IncarnationName(Abi::ComponentBehaviorAccount),
             RunIncarnation(m_context, tag++, Abi::ComponentBehaviorAccount, systemCall)))
    {
        return Abandon(systemCall);
    }
    // Where the platform grants an assignable device, a driver drives it through registers it maps
    // into its own window.
    if (((m_bootstrap->ManagementFeatures & Abi::PagerManagementDevices) != 0U) &&
        m_devices.IsPresent(Abi::PagerDeviceEdu) &&
        ((m_bootstrap->ManagementFeatures & Abi::PagerManagementResourceDomains) != 0U) &&
        (m_launch->Component.WindowPages != 0U) &&
        !Ran(IncarnationName(Abi::ComponentBehaviorDriver),
             RunIncarnation(m_context, tag++, Abi::ComponentBehaviorDriver, systemCall)))
    {
        return Abandon(systemCall);
    }
    // Tags after the echo's go to later components.
    const Foundation::UInt64 echoTag = tag++;
    m_context.NextTag = tag;
    if (((m_bootstrap->ManagementFeatures & Abi::PagerManagementWaitSets) != 0U) && !RunServices(echoTag, systemCall))
    {
        return Abandon(systemCall);
    }
    // Last, the loader's demonstration, every earlier record torn down.
    if ((m_launch->LoaderDemo != nullptr) && !Ran(LoaderName, m_launch->LoaderDemo(m_context, systemCall)))
    {
        return Abandon(systemCall);
    }
    if (((m_bootstrap->ManagementFeatures & Abi::PagerManagementWaitSets) != 0U) && (m_launch->InputDemo != nullptr) &&
        !Ran("input", m_launch->InputDemo(m_context, systemCall)))
    {
        return Abandon(systemCall);
    }
    if (!Ran(DisplayName, StopDisplay(systemCall)))
    {
        return Abandon(systemCall);
    }
    if (!Ran(SharedEndpointsName, m_calls.Destroy(m_context.Report, Abi::ObjectTypeEndpoint, systemCall)
                                      ? PagerServeError::None
                                      : PagerServeError::ManagementFailed))
    {
        return Abandon(systemCall);
    }
    m_context.Report = {};
    // Last, so that a runner judging the kernel's state after it sees
    // everything the root service created destroyed.
    WriteLine(m_context, RootServiceDoneLine, sizeof(RootServiceDoneLine) - 1U);
    return MakeOutcome(PagerServeError::None, Abi::SystemCallError::None);
}

// Where components can wait, a service incarnation follows; it replies
// on an Endpoint of the root service's own.
bool TestRootService::RunServices(const Foundation::UInt64 echoTag, Abi::SystemCallError& systemCall) noexcept
{
    if (!Ran(SharedEndpointsName, m_calls.CreateEndpoint(m_bootstrap->Management, m_context.Reply, systemCall)
                                      ? PagerServeError::None
                                      : PagerServeError::ManagementFailed))
    {
        return false;
    }
    // Where a driver ran, its recovery: one component live, so a crash is attributable.
    if ((m_launch->DriverDemo != nullptr) && ((m_bootstrap->ManagementFeatures & Abi::PagerManagementDevices) != 0U) &&
        m_devices.IsPresent(Abi::PagerDeviceEdu) &&
        ((m_bootstrap->ManagementFeatures & Abi::PagerManagementResourceDomains) != 0U) &&
        (m_launch->Component.WindowPages != 0U) &&
        !Ran(DriverRecoveryName, m_launch->DriverDemo(m_context, systemCall)))
    {
        return false;
    }
    // Where the platform grants its AHCI controller, the filesystem service's demonstration, then
    // the block driver's. Both write to the disk, the first may format it: they run only where the
    // controller holds no disk but the emulator's.
    const DiskDemonstrations disk =
        SelectDiskDemonstrations(m_bootstrap->ManagementFeatures,
                                 ((m_bootstrap->ManagementFeatures & Abi::PagerManagementDevices) != 0U) &&
                                     m_devices.IsPresent(Abi::PagerDeviceAhci) &&
                                     ((m_bootstrap->ManagementFeatures & Abi::PagerManagementResourceDomains) != 0U) &&
                                     (m_launch->Component.WindowPages != 0U));
    if ((m_launch->FsDemo != nullptr) && (disk == DiskDemonstrations::Skipped))
    {
        WriteLine(m_context, FsSkippedLine, sizeof(FsSkippedLine) - 1U);
        if (!Ran("inventory-save", RunDeviceInventorySave(m_context, systemCall)))
        {
            return false;
        }
    }
    if ((m_launch->FsDemo != nullptr) && (disk == DiskDemonstrations::Run) &&
        !Ran(FsName, m_launch->FsDemo(m_context, systemCall)))
    {
        return false;
    }
    // Where the platform grants its NIC, the network demonstration, after the filesystem's services
    // are torn down and before the block demonstration, whose evidence is the last pins the Device
    // Manager releases.
    if ((m_launch->NetDemo != nullptr) && ((m_bootstrap->ManagementFeatures & Abi::PagerManagementDevices) != 0U) &&
        m_devices.IsPresent(Abi::PagerDeviceNic) &&
        ((m_bootstrap->ManagementFeatures & Abi::PagerManagementResourceDomains) != 0U) &&
        (m_launch->Component.WindowPages != 0U) && !Ran(NetName, m_launch->NetDemo(m_context, systemCall)))
    {
        return false;
    }
    if ((m_launch->BlockDemo != nullptr) && (disk == DiskDemonstrations::Skipped))
    {
        WriteLine(m_context, BlockSkippedLine, sizeof(BlockSkippedLine) - 1U);
    }
    if ((m_launch->BlockDemo != nullptr) && (disk == DiskDemonstrations::Run) &&
        !Ran(BlockName, m_launch->BlockDemo(m_context, systemCall)))
    {
        return false;
    }
    if ((m_launch->PortsDemo != nullptr) && !Ran("ports", m_launch->PortsDemo(m_context, systemCall)))
    {
        return false;
    }
    if (!Ran(EchoName, RunEcho(m_context, echoTag, systemCall)))
    {
        return false;
    }
    // Then the services side by side, where the image has the Directory.
    if ((m_launch->ServiceDemo != nullptr) && (m_context.Directory.Bytes != nullptr) &&
        !Ran(ServicesName, m_launch->ServiceDemo(m_context, systemCall)))
    {
        return false;
    }
    if (!Ran(SharedEndpointsName, m_calls.Destroy(m_context.Reply, Abi::ObjectTypeEndpoint, systemCall)
                                      ? PagerServeError::None
                                      : PagerServeError::ManagementFailed))
    {
        return false;
    }
    m_context.Reply = {};
    return true;
}

PagerServeOutcome RunTestRootService(const System::SystemCallClient& client, const PagerBootstrap& bootstrap,
                                     PagerProgram& pager, const PagerThreadLaunch& launch) noexcept
{
    static constinit TestRootService root{};
    return root.Run(client, bootstrap, pager, launch);
}
} // Namespace Asteria::User::Pager
