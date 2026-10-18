// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_PAGER_BOOTSTRAP_HPP_INCLUDED
#define ASTERIA_ABI_PAGER_BOOTSTRAP_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Abi/Watchdog.hpp"
#include "Abi/DesktopSession.hpp"

namespace Asteria::Abi
{
/// The boot pager's start block: PagerBootstrapWords little-endian words the kernel writes at the
/// top of the pager's initial stack and passes by address. Six handles of selector and
/// authenticator (the pager Endpoint with Read, its WaitSet with Wait, a DiagnosticChannel with
/// Write, the client AddressSpace with Map, and the lazy region's MemoryObject and Frame with Map),
/// then the client virtual address and page count of the lazy region the pager backs, then a
/// ResourceDomain handle with Manage, Inspect and Code that ObjectCreate charges, or zero where the
/// kernel grants none, then the management features that grant offers (PagerManagement* bits) and a
/// zero word. With PagerManagementMapping, the pager's own AddressSpace with Map follows, then the
/// virtual start and page count of a window of that space nothing maps, where MemoryMap may place
/// created memory; without it those four words are zero. A device table of three entries of four
/// words follows, indexed by the kind of device (PagerDevice*): an assignable PCI function
/// (Inspect, Invoke, Manage, Duplicate, Transfer) and the memory-mapped DeviceRegion of its BAR
/// (Inspect, Map, Read, Write, Duplicate, Transfer). With PagerManagementDevices at least one entry
/// names a device; an entry naming none is zero. The scanout follows: the boot's framebuffer as a
/// DeviceRegion (Inspect, Map, Read, Write, Duplicate, Transfer) and its mode, width | height << 32
/// and pixels per scan line | format << 32 (Boot::BootDisplayFormat*); four zero words without one.
/// The next eight words hold the i8042 data and command DeviceRegions (Read/Write), then keyboard
/// and mouse InterruptLines (Invoke), as selector/authenticator pairs; zero when unpublished. The
/// last three inventory words hold a read-only Frame handle and its page count, followed by the
/// module table virtual base and byte size. Appended after those original 51 words are four xHCI
/// function/region pairs, the calibrated clock ticks per microsecond and the i8042 declaration,
/// then four function/region pairs of USB 1.1/2.0 host controllers (EHCI, OHCI, UHCI), watchdog
/// seconds, SessionMode and LoginEnabled.
inline constexpr Foundation::Size PagerBootstrapWords{88U};
inline constexpr Foundation::Size PagerBootstrapEndpointWord{0U};
inline constexpr Foundation::Size PagerBootstrapWaitSetWord{2U};
inline constexpr Foundation::Size PagerBootstrapDiagnosticWord{4U};
inline constexpr Foundation::Size PagerBootstrapClientSpaceWord{6U};
inline constexpr Foundation::Size PagerBootstrapLazyMemoryWord{8U};
inline constexpr Foundation::Size PagerBootstrapLazyFrameWord{10U};
inline constexpr Foundation::Size PagerBootstrapLazyStartWord{12U};
inline constexpr Foundation::Size PagerBootstrapLazyPagesWord{13U};
inline constexpr Foundation::Size PagerBootstrapManagementWord{14U};
inline constexpr Foundation::Size PagerBootstrapManagementFeaturesWord{16U};
inline constexpr Foundation::Size PagerBootstrapOwnSpaceWord{18U};
inline constexpr Foundation::Size PagerBootstrapWindowStartWord{20U};
inline constexpr Foundation::Size PagerBootstrapWindowPagesWord{21U};
inline constexpr Foundation::Size PagerBootstrapDeviceTableWord{22U};
inline constexpr Foundation::Size PagerBootstrapDevices{11U};
/// The device table's entries: QEMU's `edu`, the test device of the isolated driver, the AHCI
/// controller once the boot's own commands ran, and the e1000e NIC's function and register BAR,
/// then up to four xHCI controllers in PCI catalog order, then up to four USB 1.1/2.0 host
/// controllers in PCI catalog order, whole groups of an EHCI and the companion UHCI/OHCI functions
/// of its PCI device: the function and its register BAR, memory for EHCI and OHCI, I/O ports for
/// UHCI.
inline constexpr Foundation::Size PagerDeviceEdu{0U};
inline constexpr Foundation::Size PagerDeviceAhci{1U};
inline constexpr Foundation::Size PagerDeviceNic{2U};
inline constexpr Foundation::Size PagerDeviceXhci{3U};
inline constexpr Foundation::Size PagerXhciControllers{4U};
inline constexpr Foundation::Size PagerDeviceUsbHost{PagerDeviceXhci + PagerXhciControllers};
inline constexpr Foundation::Size PagerUsbHostControllers{4U};
static_assert(PagerDeviceUsbHost + PagerUsbHostControllers == PagerBootstrapDevices);

inline constexpr Foundation::Size PagerBootstrapXhciTableWord{51U};
inline constexpr Foundation::Size PagerBootstrapUsbHostTableWord{69U};

/// The words of device's function and region handles.
[[nodiscard]] constexpr Foundation::Size PagerBootstrapDeviceFunctionWord(const Foundation::Size device) noexcept
{
    return device < PagerDeviceXhci
               ? PagerBootstrapDeviceTableWord + (device << 2U)
               : (device < PagerDeviceUsbHost ? PagerBootstrapXhciTableWord + ((device - PagerDeviceXhci) << 2U)
                                              : PagerBootstrapUsbHostTableWord + ((device - PagerDeviceUsbHost) << 2U));
}

[[nodiscard]] constexpr Foundation::Size PagerBootstrapDeviceRegionWord(const Foundation::Size device) noexcept
{
    return PagerBootstrapDeviceFunctionWord(device) + 2U;
}

inline constexpr Foundation::Size PagerBootstrapScanoutRegionWord{34U};
inline constexpr Foundation::Size PagerBootstrapScanoutModeWord{36U};

static_assert(PagerBootstrapDeviceRegionWord(PagerDeviceNic) + 2U == PagerBootstrapScanoutRegionWord);
inline constexpr Foundation::Size PagerBootstrapI8042DataWord{38U};
inline constexpr Foundation::Size PagerBootstrapI8042CommandWord{40U};
inline constexpr Foundation::Size PagerBootstrapI8042KeyboardWord{42U};
inline constexpr Foundation::Size PagerBootstrapI8042MouseWord{44U};
static_assert(PagerBootstrapScanoutModeWord + 2U == PagerBootstrapI8042DataWord);
inline constexpr Foundation::Size PagerBootstrapInventoryFrameWord{46U};
inline constexpr Foundation::Size PagerBootstrapInventoryPagesWord{48U};
static_assert(PagerBootstrapI8042MouseWord + 2U == PagerBootstrapInventoryFrameWord);
inline constexpr Foundation::Size PagerBootstrapModuleBaseWord{49U};
inline constexpr Foundation::Size PagerBootstrapModuleBytesWord{50U};
static_assert(PagerBootstrapInventoryPagesWord + 1U == PagerBootstrapModuleBaseWord);
static_assert(PagerBootstrapModuleBaseWord + 1U == PagerBootstrapModuleBytesWord);
static_assert(PagerBootstrapModuleBytesWord + 1U == PagerBootstrapXhciTableWord);
inline constexpr Foundation::Size PagerBootstrapClockTicksPerMicrosecondWord{67U};
static_assert(PagerBootstrapDeviceRegionWord(PagerDeviceUsbHost - 1U) + 2U ==
              PagerBootstrapClockTicksPerMicrosecondWord);
inline constexpr Foundation::Size PagerBootstrapI8042DeclaredWord{68U};
static_assert(PagerBootstrapClockTicksPerMicrosecondWord + 1U == PagerBootstrapI8042DeclaredWord);
static_assert(PagerBootstrapI8042DeclaredWord + 1U == PagerBootstrapUsbHostTableWord);
inline constexpr Foundation::Size PagerBootstrapWatchdogSecondsWord{85U};
inline constexpr Foundation::Size PagerBootstrapSessionModeWord{86U};
inline constexpr Foundation::Size PagerBootstrapLoginEnabledWord{87U};
static_assert(PagerBootstrapLoginEnabledWord + 1U == PagerBootstrapWords);
static_assert(PagerBootstrapDeviceRegionWord(PagerBootstrapDevices - 1U) + 2U == PagerBootstrapWatchdogSecondsWord);
/// Read-only module table, retained for the system lifetime.
inline constexpr Foundation::UInt64 PagerModuleMaximumBytes{2U * 1024U * 1024U};
inline constexpr Foundation::UInt64 Amd64PagerModuleBase{0x4400'0000ULL};
inline constexpr Foundation::UInt64 Ia64PagerModuleBase{0x2000'0000'0700'0000ULL};
/// The module of that table (BOOT.CFG `module = component...`) every driver component runs from:
/// the root service's component image. A BOOT.CFG trust entry must name it, so the digest the
/// loader verifies is that of the code a trusted driver runs.
inline constexpr char PagerDriverModuleName[] = "component";
/// The module the USB 1.1/2.0 host group driver runs from (behaviour
/// ComponentBehaviorXhci with UsbHostGroupFlag), the `usb` image, which a
/// BOOT.CFG trust entry for the usb driver names in its place.
inline constexpr char PagerUsbHostModuleName[] = "usb";
/// The kernel has a frame pool, so ObjectCreate makes Frames.
inline constexpr Foundation::UInt64 PagerManagementFrames{1U};
/// MemoryMap maps created memory, and the start block names the pager's own space and a free window
/// of it.
inline constexpr Foundation::UInt64 PagerManagementMapping{2U};
/// ObjectCreate makes threads in the pager's own domain.
inline constexpr Foundation::UInt64 PagerManagementThreads{4U};
/// ObjectCreate makes AddressSpaces, CapabilitySpaces and ProtectionDomains, and threads in them:
/// the pager may build components.
inline constexpr Foundation::UInt64 PagerManagementDomains{8U};
/// ObjectCreate makes WaitSets, WaitSetBind binds created Endpoints to them, and the pager's own
/// WaitSet capability carries Manage.
inline constexpr Foundation::UInt64 PagerManagementWaitSets{16U};
/// ObjectCreate makes child ResourceDomains of the grant and ObjectDestroy drains them, so the root
/// service charges each component to a domain of its own.
inline constexpr Foundation::UInt64 PagerManagementResourceDomains{32U};
/// The start block's device table names an assignable device: a PCI function and the DeviceRegion
/// of its BAR, which the root service, as Device Manager, gives a driver.
inline constexpr Foundation::UInt64 PagerManagementDevices{64U};
/// The device table's AHCI controller holds no disk but the emulator's: the
/// boot found no disk, or its IDENTIFY DEVICE named QEMU's or the VM runner's
/// model (Kernel/Device/AhciEmulatorDisk.hpp). Only then does the root
/// service run its demonstrations that write to or format the disk, which
/// otherwise may be real.
inline constexpr Foundation::UInt64 PagerManagementDiskWritable{128U};

/// The tag of the Notification the kernel signals after delivering a fault;
/// the pager's WaitSet wait returns it in value 0.
inline constexpr Foundation::UInt64 PagerReadinessTag{0x5041'4745'5245'4144ULL};

/// The first word of every lazy page, followed by the page's index, so a
/// Client that touches the region can check it reads the pager's frame.
inline constexpr Foundation::UInt64 LazyPagePattern{0x4C41'5A59'5041'4745ULL};

/// The first word the pager writes to each page it maps through MemoryMap,
/// followed by the page's index.
inline constexpr Foundation::UInt64 MappedPagePattern{0x4D41'5050'4544'5047ULL};
} // Namespace Asteria::Abi

#endif // ASTERIA_ABI_PAGER_BOOTSTRAP_HPP_INCLUDED
