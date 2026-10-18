// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_PAGER_ROOT_SCENARIOS_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_PAGER_ROOT_SCENARIOS_HPP_INCLUDED

#include "User/Pager/RootContext.hpp"

namespace Asteria::User::Pager
{
/// The lines the root service writes (TAP comments) as it tears down an
/// incarnation that crashed, one its watchdog killed, or one that reported.
inline constexpr char ComponentCrashedLine[] = "# component: crashed, terminated and torn down\n";
inline constexpr char ComponentHungLine[] = "# component: hung, killed and torn down\n";
inline constexpr char ComponentReportedLine[] = "# component: reported, exited and torn down\n";
/// The line of an incarnation that created and mapped memory in its own ResourceDomain and was
/// refused past its pages, drained.
inline constexpr char ComponentAccountLine[] =
    "# component: created in its own domain, refused beyond it and executable memory, drained\n";
/// The line of a driver that mapped the assignable device's registers and drove it, then confined
/// its DMA in an IommuDomain and saw a stray write fault without landing, waiting for its device's
/// MSIs, drained.
inline constexpr char ComponentDriverLine[] =
    "# driver: 10! computed, DMA confined, stray write faulted, MSIs waited, drained\n";
/// The line of a driver on a kernel without an IOMMU, which
/// drove its device through its registers only, its bus mastering refused,
/// and waited for its device's INTx line.
inline constexpr char ComponentDriverIntxLine[] =
    "# driver: 10! computed, DMA confined, stray write faulted, INTx waited, drained\n";
inline constexpr char ComponentDriverRegistersLine[] =
    "# driver: 10! computed, bus master refused without an IOMMU, INTx waited, drained\n";
/// The line of the driver's recovery: its service crashed with
/// a transfer running and was drained and restarted, and the restarted one
/// answered the outstanding request DriverRestarted and served it again.
inline constexpr char DriverRecoveredLine[] = "# driver: crashed mid-transfer, restarted, DriverRestarted, served\n";
/// Its recovery on a device that may not master the bus, whose
/// service crashed with a factorial running and its interrupt owed.
inline constexpr char DriverFactorialRecoveredLine[] =
    "# driver: crashed mid-factorial, restarted, DriverRestarted, served\n";
/// The lines of the AHCI driver's demonstration: it read the disk's token, had a queued read
/// Cancelled and a finished one's Cancel answered TooLate, wrote a sector and read it back, and
/// crashed with a read running into a Frame its client destroyed, the controller reset before the
/// drain's quarantine lifted; or, without a disk, served nothing and was killed and drained.
inline constexpr char BlockServedLine[] =
    "# block: token read, queued read Cancelled, TooLate, written and read back, crashed reading, Frame destroyed, reset, drained\n";
/// The block demonstration's request to the runner to throttle the disk
/// before the read the driver crashes on.
inline constexpr char BlockThrottleLine[] = "# block: throttle the disk for the crash's read\n";
inline constexpr char BlockNoDiskLine[] = "# block: no disk, queued read Cancelled, served nothing, drained\n";
/// The line of an echo incarnation the root service killed while it waited for its next request.
inline constexpr char ComponentEchoedLine[] = "# component: echoed, killed while waiting and torn down\n";
/// The filesystem demonstration's lines: a FAT volume read and written, then, where its readme's
/// scenario asks, a commit crashed before its directory entry; an exFAT volume read and written;
/// the blank volume formatted as AsteriaFS, written, and crashed on either side of a commit's
/// superblock; or no disk.
inline constexpr char FsFatLine[] = "# fs: fat volume, readme.txt read, hello committed and read back\n";
inline constexpr char FsFatCrashedLine[] =
    "# fs: fat volume, readme.txt read, hello committed and read back, crash before the entry, restarted, crash.txt not found\n";
inline constexpr char FsExfatLine[] = "# fs: exfat volume, readme.txt read, hello committed and read back\n";
inline constexpr char FsAsteriaLine[] =
    "# fs: asteriafs formatted, hello committed and read back, crash before the superblock NotCommitted, crash after Committed\n";
inline constexpr char FsNoDiskLine[] = "# fs: no disk, driver killed while it waits\n";
/// Written in place of the filesystem and block demonstrations, which write to and may format the
/// disk, when the boot did not identify the disk as the emulator's (Abi:
/// PagerManagementDiskWritable): a real disk is never touched.
inline constexpr char FsSkippedLine[] = "# fs: skipped, the disk is not the emulator's\n";
inline constexpr char BlockSkippedLine[] = "# block: skipped, the disk is not the emulator's\n";

/// What becomes of the demonstrations that write to the AHCI controller's
/// disk: none without a granted controller; skipped, with a line, unless the
/// start block says the controller holds no disk but the emulator's.
enum class DiskDemonstrations : Foundation::UInt8
{
    None,
    Skipped,
    Run,
};

[[nodiscard]] constexpr DiskDemonstrations SelectDiskDemonstrations(const Foundation::UInt64 managementFeatures,
                                                                    const bool controllerGranted) noexcept
{
    if (!controllerGranted)
    {
        return DiskDemonstrations::None;
    }
    return ((managementFeatures & Abi::PagerManagementDiskWritable) != 0U) ? DiskDemonstrations::Run
                                                                           : DiskDemonstrations::Skipped;
}
/// The root service's line once the display broker is torn down, written to the debug channel only:
/// a runner compares the screen with the lines from the broker's first line, "# display:
/// <width>x<height> <rgbx|bgrx>, console <columns>x<rows>", up to this one.
inline constexpr char InputDemonstratedLine[] =
    "# input: typed 'asteria', pointer moved (5,-3), left button, secure attention reported, focus switched\n";
inline constexpr char DisplayStoppedLine[] = "# display: stopped\n";
/// The root service's last line, once it destroyed everything it created:
/// a runner judges the kernel's state only after it.
inline constexpr char RootServiceDoneLine[] = "# root service: done\n";
/// The lines of the services the root service runs side by side: the
/// Directory bound `echo` for it, and both were killed while they waited.
inline constexpr char DirectoryBoundLine[] = "# directory: bound echo\n";
/// The lines of the client that resolved `echo` through the Directory and
/// got its echoes, of Echo restarted in a fresh domain and rebound
///, and of the client whose send through the capability it resolved
/// first was refused, which resolved `echo` again, got its echoes and exited.
inline constexpr char ClientResolvedLine[] = "# client: resolved echo and got 3 echoes\n";
inline constexpr char EchoReboundLine[] = "# directory: echo restarted and rebound\n";
inline constexpr char ClientRecoveredLine[] = "# client: stale send refused, resolved again, 3 echoes, torn down\n";
inline constexpr char ServicesStoppedLine[] = "# services: directory and echo killed while waiting and torn down\n";
/// The lines of the loader's demonstration: an IA-64 executable ran under the interpreter
/// registered for it, an AArch64 one found none, and a PE32+ one was refused as a format the native
/// loader does not support.
inline constexpr char LoaderAmd64InterpretedLine[] =
    "# loader: amd64 executable ran under its registered interpreter\n";
inline constexpr char LoaderInterpretedLine[] = "# loader: ia64 executable ran under its registered interpreter\n";
inline constexpr char LoaderForeignLine[] = "# loader: aarch64 executable refused, no interpreter registered\n";
inline constexpr char LoaderFormatLine[] = "# loader: pe32+ executable refused as an unsupported format\n";

/// What the component's incarnations do, in order: more incarnations than the platform has arenas,
/// so each reuses a dead domain's slots. Incarnation n is supervised under tag n.
inline constexpr Foundation::UInt64 ComponentIncarnations[]{
    Abi::ComponentBehaviorCrash,
    Abi::ComponentBehaviorHang,
    Abi::ComponentBehaviorReport,
};

/// Builds, supervises and tears down one incarnation of the component.
[[nodiscard, gnu::noinline]] PagerServeError RunIncarnation(ScenarioContext& context, Foundation::UInt64 incarnation,
                                                            Foundation::UInt64 behavior,
                                                            Abi::SystemCallError& systemCall) noexcept;
/// Builds an echo incarnation tagged tag, exchanges one request with it,
/// kills it while it waits, reclaims it and tears it down.
[[nodiscard, gnu::noinline]] PagerServeError RunEcho(ScenarioContext& context, Foundation::UInt64 tag,
                                                     Abi::SystemCallError& systemCall) noexcept;

/// The PagerServiceDemo of a platform whose pager embeds the Directory
/// (ServiceDemo.cpp): the Directory and Echo as services, `echo` bound.
[[nodiscard]] PagerServeError RunServiceDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;

/// The PagerLoaderDemo of a platform whose pager embeds an interpreter and
/// foreign executables (LoaderDemo.cpp): one interpreted launch and two
/// refusals.
[[nodiscard]] PagerServeError RunLoaderDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;

/// The PagerDriverDemo of a platform that grants an assignable device and
/// an IOMMU (DriverDemo.cpp): a driver service killed mid-transfer and
/// restarted, which answers the outstanding request with DriverRestarted.
[[nodiscard]] PagerServeError RunDriverDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;
/// The same of a platform whose device may not master the bus
///: the service crashes with a factorial running.
[[nodiscard]] PagerServeError RunFactorialDriverDemo(ScenarioContext& context,
                                                     Abi::SystemCallError& systemCall) noexcept;

/// The PagerBlockDemo of a platform that grants its AHCI controller (BlockDemo.cpp,
/// Source/User/System/AhciPort.hpp ): the root service as a client of the AHCI driver service.
[[nodiscard]] PagerServeError RunBlockDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;
/// The PagerFsDemo of a platform that grants its AHCI controller (FsDemo.cpp,
/// Documentation/Architecture/FilesystemProtocol.md ): the root service as a client of the
/// filesystem service on an AHCI driver, before the block demonstration.
[[nodiscard]] PagerServeError RunDeviceInventorySave(ScenarioContext& context,
                                                     Abi::SystemCallError& systemCall) noexcept;

[[nodiscard]] PagerServeError RunFsDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;
/// Scripted input, after all other scenarios have torn down, beside display.
[[nodiscard]] PagerServeError RunInputDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;
/// The USB host group driver's recovery, where the USB host table holds an EHCI: typed input
/// through the driver, then four kills with its interrupt polls queued, each recovered by the
/// product's RecoverInputDrivers: restarted three times (typed input again after the first), given
/// up after the fourth, its controllers proven quiet and released every time.
[[nodiscard]] PagerServeError RunUsbHostRecoveryDemo(ScenarioContext& context,
                                                     Abi::SystemCallError& systemCall) noexcept;
/// The PagerNetDemo of a platform that grants its NIC (NetDemo.cpp,
/// Source/User/Network/NetworkStack.hpp ): the root service starts the NIC driver and is its
/// client.
[[nodiscard]] PagerServeError RunNetDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;
[[nodiscard]] PagerServeError RunPortsDemo(ScenarioContext& context, Abi::SystemCallError& systemCall) noexcept;

} // Namespace Asteria::User::Pager

#endif // ASTERIA_TESTS_GUEST_PAGER_ROOT_SCENARIOS_HPP_INCLUDED
