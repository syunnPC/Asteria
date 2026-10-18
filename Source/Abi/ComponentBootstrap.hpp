// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_ABI_COMPONENT_BOOTSTRAP_HPP_INCLUDED
#define ASTERIA_ABI_COMPONENT_BOOTSTRAP_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Abi
{
/// The component the root service builds in a domain it creates. The root service maps the stack
/// pages and the start-block page right above them read-write in the component's space, and starts
/// its thread with the stack top below the start block and the start block's address as the
/// argument. Each pager image names its ISA's stack (User/Pager/PagerProgram.hpp,
/// ComponentPlatform); AMD64's is ComponentStackPages pages from ComponentStackStart, with the
/// block at ComponentStartBlockAddress. The block's 16 little-endian words: the magic, the
/// incarnation (counting from 1), the Endpoint the component reports on (selector, authenticator;
/// Invoke only), the ticks of the ISA's counter (the TSC, the ITC) a crashing incarnation spins
/// before it executes an instruction its ISA refuses, what the incarnation does
/// (ComponentBehavior*), then up to four more handles, each zero where the component gets none: its
/// WaitSet (Wait), the Endpoint it receives on, its service's or its replies' (Read), a send
/// capability on the Directory (Invoke), and a send capability on the Endpoint it receives on, for
/// the reply attachments of its requests (Invoke and Duplicate); then how a crashing incarnation
/// crashes (ComponentCrash*; extended processor state preservation ) and a zero word.
inline constexpr Foundation::UInt64 ComponentStackStart{0x5000'1000U};
inline constexpr Foundation::UInt64 ComponentStackPages{2U};
inline constexpr Foundation::UInt64 ComponentStartBlockAddress{ComponentStackStart + (ComponentStackPages * 4096U)};
/// IA-64's: the component lives in region 1 (a user region), its segments in
/// [Ia64ComponentUserStart, Ia64ComponentImageLimit) and its 32 KiB stack
/// region (the RSE backing store, then the memory stack) from
/// Ia64ComponentStackStart, with the start block above.
inline constexpr Foundation::UInt64 Ia64ComponentUserStart{0x2000'0000'0000'1000ULL};
inline constexpr Foundation::UInt64 Ia64ComponentUserLimit{0x2000'0000'8000'0000ULL};
inline constexpr Foundation::UInt64 Ia64ComponentStackStart{0x2000'0000'0200'0000ULL};
inline constexpr Foundation::UInt64 Ia64ComponentStackPages{8U};
/// IA-64's window, clear of the segments' start and the stack region in region 1.
inline constexpr Foundation::UInt64 Ia64ComponentWindowStart{0x2000'0000'0400'0000ULL};
inline constexpr Foundation::UInt64 Ia64ComponentWindowPages{2U};
/// Read-only foreign executable bytes staged for a native interpreter.
inline constexpr Foundation::UInt64 Ia64ComponentForeignImageAddress{0x2000'0000'0600'0000ULL};
/// Images occupy the low part of region 1, below the stack reservation.
inline constexpr Foundation::UInt64 Ia64ComponentImageLimit{Ia64ComponentStackStart};
/// Reserved shared code range [Start, Limit). Shared segments use fixed image addresses.
inline constexpr Foundation::UInt64 Ia64ComponentSharedCodeStart{0x2000'0000'1000'0000ULL};
inline constexpr Foundation::UInt64 Ia64ComponentSharedCodeLimit{0x2000'0000'2000'0000ULL};
static_assert(Ia64ComponentUserStart < Ia64ComponentImageLimit);
static_assert(Ia64ComponentImageLimit <= Ia64ComponentSharedCodeStart);
static_assert(Ia64ComponentStackStart + ((Ia64ComponentStackPages + 1U) * 4096U) <= Ia64ComponentSharedCodeStart);
static_assert(Ia64ComponentWindowStart + (Ia64ComponentWindowPages * 4096U) <= Ia64ComponentForeignImageAddress);
static_assert(Ia64ComponentSharedCodeStart < Ia64ComponentSharedCodeLimit);
static_assert(Ia64ComponentSharedCodeLimit < Ia64ComponentUserLimit);
inline constexpr Foundation::Size ComponentStartBlockWords{16U};
inline constexpr Foundation::Size ComponentStartMagicWord{0U};
inline constexpr Foundation::Size ComponentIncarnationWord{1U};
inline constexpr Foundation::Size ComponentReportWord{2U};
inline constexpr Foundation::Size ComponentSpinTicksWord{4U};
inline constexpr Foundation::Size ComponentBehaviorWord{5U};
inline constexpr Foundation::Size ComponentWaitSetWord{6U};
inline constexpr Foundation::Size ComponentReceiveWord{8U};
inline constexpr Foundation::Size ComponentDirectoryWord{10U};
inline constexpr Foundation::Size ComponentReplyWord{12U};
inline constexpr Foundation::Size ComponentCrashKindWord{14U};
inline constexpr Foundation::UInt64 ComponentStartMagic{0x434F'4D50'4F4E'4E54ULL};

/// What an incarnation does: spin its ticks and crash, report and exit, or hang in a CPU-bound loop
/// that never reports; or, as a service, wait on its WaitSet and echo each request until it is
/// killed.
// Independent-supervisor VM proof, enabled only by the test module receipt.
inline constexpr Foundation::UInt64 ComponentBehaviorFaultSupervisor{24U};
// Separate root-started authentication and login modules.
inline constexpr Foundation::UInt64 ComponentBehaviorAuth{25U};
inline constexpr Foundation::UInt64 ComponentBehaviorLogin{26U};
inline constexpr Foundation::UInt64 ComponentFaultSupervisorExitCode{0xC0DE0024U};
inline constexpr Foundation::UInt64 ComponentFaultSupervisorFailedExitCode{0xC0DE00F3U};
inline constexpr Foundation::UInt64 ComponentBehaviorCrash{1U};
inline constexpr Foundation::UInt64 ComponentBehaviorReport{2U};
inline constexpr Foundation::UInt64 ComponentBehaviorHang{3U};
inline constexpr Foundation::UInt64 ComponentBehaviorEcho{4U};
/// The Object Directory (its own image, `directory`), a service.
inline constexpr Foundation::UInt64 ComponentBehaviorDirectory{5U};
/// A client of the services (its own image, `client`), a task.
inline constexpr Foundation::UInt64 ComponentBehaviorClient{6U};
/// An interpreter given an executable for another machine or system, a task.
inline constexpr Foundation::UInt64 ComponentBehaviorInterpret{7U};
/// A task that creates objects in its own ResourceDomain and maps memory into its own window.
inline constexpr Foundation::UInt64 ComponentBehaviorAccount{8U};
/// A task that drives the assignable device through the registers it maps into its own window.
inline constexpr Foundation::UInt64 ComponentBehaviorDriver{9U};
/// The driver as a service its Device Manager restarts: it quiesces what a dead instance left
/// running, then serves copies through the device (User/Component/DriverFrame.hpp) until killed.
inline constexpr Foundation::UInt64 ComponentBehaviorDriverService{10U};
/// The AHCI driver, a service its Device Manager restarts (Documentation/
/// Source/User/System/AhciPort.hpp ): it brings up the controller's first port with a disk and
/// Serves the block protocol (User/Component/ BlockFrame.hpp) until killed. Its WaitSet carries
/// Manage, so it binds its Interrupt beside its Endpoint.
inline constexpr Foundation::UInt64 ComponentBehaviorBlockDriver{11U};
/// A 32-bit AHCI controller uses a driver-owned 128-sector bounce run; client data is mapped
/// separately for copying.
inline constexpr Foundation::UInt64 AhciBouncePages{16U};
inline constexpr Foundation::UInt64 AhciBouncePage{2U};
inline constexpr Foundation::UInt64 AhciClientPage{AhciBouncePage + AhciBouncePages};
inline constexpr Foundation::UInt64 AhciWindowPages{AhciClientPage + AhciBouncePages};
static_assert(AhciBouncePages * 4096U == 128U * 512U);
/// The filesystem service: it mounts the volume of the block service its start block names and
/// serves the file protocol (User/Component/FsFrame.hpp) until killed.
inline constexpr Foundation::UInt64 ComponentBehaviorFilesystem{12U};
/// The System Display Broker: it maps the scanout its start block names, clears it and draws the
/// system console the console protocol (User/Component/DisplayFrame.hpp) writes, until killed.
inline constexpr Foundation::UInt64 ComponentBehaviorDisplay{13U};
/// Root-started test-only session probe.
inline constexpr Foundation::UInt64 ComponentBehaviorSessionProbe{21U};
inline constexpr Foundation::UInt64 ComponentSessionProbePages{63U};
/// Separate compositor and test pattern client.
inline constexpr Foundation::UInt64 ComponentBehaviorCompositor{22U};
inline constexpr Foundation::UInt64 ComponentBehaviorPatternClient{23U};
/// Root-started toolkit preview.
inline constexpr Foundation::UInt64 ComponentBehaviorToolkit{27U};
inline constexpr Foundation::UInt64 ComponentBehaviorMaximum{ComponentBehaviorToolkit};
inline constexpr char ComponentToolkitPreviewModule[]{"toolkit-session"};
inline constexpr char ComponentToolkitDemoName[]{"toolkit-demo"};
// Small outputs are selected explicitly by the speed-oriented session tests.
inline constexpr Foundation::UInt32 ComponentSessionTestWidth{136U};
inline constexpr Foundation::UInt32 ComponentSessionTestHeight{112U};
inline constexpr Foundation::UInt64 ComponentCompositorPages{15U};
static_assert(ComponentCompositorPages ==
              (ComponentSessionTestWidth * ComponentSessionTestHeight * 4U + 4095U) / 4096U);
inline constexpr Foundation::UInt64 ComponentSessionTestSmallOutputFlag{1ULL << 33U};
// Compositor/pattern start blocks carry their selected output mode here;
// display uses the same word for its scanout address.
inline constexpr Foundation::Size ComponentSessionOutputModeWord{42U};
inline constexpr Foundation::UInt64 ComponentPatternPages{2U * ComponentCompositorPages};
inline constexpr Foundation::UInt64 ComponentPatternSecondaryPages{ComponentPatternPages};
// Every small-test image fits the complete output.
inline constexpr Foundation::UInt64 ComponentPatternInputPages{4U * ComponentCompositorPages};
static_assert(ComponentPatternInputPages * 4096U >= 4U * ComponentSessionTestWidth * ComponentSessionTestHeight * 4U);
inline constexpr Foundation::UInt32 ComponentPatternInputInitialWidth{56U};
inline constexpr Foundation::UInt32 ComponentPatternInputHeight{16U};
inline constexpr Foundation::UInt32 ComponentPatternInputMaximumWidth{64U};
static_assert(ComponentPatternInputMaximumWidth * ComponentPatternInputHeight * 4U <= 4096U);
/// The compositor's test start block reuses the device handle words for
/// Invoke on a diagnostic report Endpoint; it has no device or launch authority.
inline constexpr Foundation::Size ComponentSessionReportWord{30U};
/// The root/CM registers the seat with BrokerPointer and grants the compositor Invoke|Duplicate on
/// that client, never broker Manage.
inline constexpr Foundation::Size ComponentSessionInputWord{38U};
inline constexpr Foundation::Size ComponentSessionInputEpochWord{40U};
// UI data modules. Borrow Map only on the extended
// stack Frame and its MemoryObject. Font bytes occupy spare page offsets, never the writable stack
// virtual range.
inline constexpr Foundation::Size ComponentUiFontFrameWord{45U};
inline constexpr Foundation::Size ComponentUiFontMemoryWord{47U};
inline constexpr Foundation::Size ComponentUiFontPageWord{49U};
inline constexpr Foundation::Size ComponentUiFontBoldBytesWord{50U};
inline constexpr Foundation::Size ComponentUiFontRegularBytesWord{51U};
inline constexpr Foundation::UInt64 ComponentUiFontFaceBytes{16384U};
inline constexpr Foundation::UInt64 ComponentUiFontPages{8U};
static_assert(ComponentUiFontPages * 4096U == 2U * ComponentUiFontFaceBytes);
inline constexpr char ComponentUiBoldModule[]{"ui-bold"};
inline constexpr char ComponentUiRegularModule[]{"ui-regular"};
/// Test-only second independent client in PATTERN.ELF, using one process
/// so the five-domain native limit still admits the hardware seat.
inline constexpr Foundation::Size ComponentPatternSecondClientWord{38U};
/// Test pattern client: Map on its existing bootstrap MemoryObject, whose
/// spare offsets cover its own pool without consuming another object slot.
inline constexpr Foundation::Size ComponentPatternMemoryWord{32U};
inline constexpr Foundation::Size ComponentPatternMemoryOffsetWord{10U};
/// The NIC driver, a service its Device Manager restarts: it resets the e1000e its device words
/// name (words 30-33), confines the function in an IommuDomain with its own descriptor and packet
/// pages, brings the link up, takes the MSI through an Interrupt bound beside its Endpoint (its
/// WaitSet carries Manage), and serves the frame protocol (User/Component/NetFrame.hpp), copying
/// each packet between a client's Frame and its own pages, until killed. Its window (words 28 and
/// 29) is NicWindowPages pages laid out as below.
inline constexpr Foundation::UInt64 ComponentBehaviorNicDriver{14U};
/// The network stack service: a client of the NIC driver its start block names (words 34-39, as a
/// filesystem service's block service), it answers ARP and ICMP echo for its address and serves UDP
/// to its clients through the socket protocol (User/Component/SocketFrame.hpp) under the rules the
/// holder of Manage on its Endpoint installs, until killed.
inline constexpr Foundation::UInt64 ComponentBehaviorNetwork{15U};
/// Hardware-free Input Broker and its event-consuming client.
inline constexpr Foundation::UInt64 ComponentBehaviorInput{16U};
inline constexpr Foundation::UInt64 ComponentBehaviorInputClient{17U};
inline constexpr Foundation::UInt64 ComponentBehaviorPs2{18U};
/// A USB HID host driver: one xHCI controller of the pager's xHCI table, or,
/// with UsbHostGroupFlag in word 37, every USB 1.1/2.0 host controller of its
/// USB host table (EHCI and their UHCI/OHCI companions; Documentation/
/// Architecture/UsbHostDriver.md). Both report and deliver input alike.
inline constexpr Foundation::UInt64 ComponentBehaviorXhci{19U};
inline constexpr Foundation::Size ComponentClockTicksPerMicrosecondWord{43U};
inline constexpr Foundation::Size ComponentPs2DeclaredWord{44U};
inline constexpr Foundation::UInt64 XhciRegisterWindowPages{16U};
inline constexpr Foundation::UInt64 XhciScratchpadBudget{256U};
inline constexpr Foundation::UInt64 XhciDmaPages{68U + XhciScratchpadBudget};
// Controller arena followed by bounded report-descriptor state for 16 devices.
inline constexpr Foundation::UInt64 XhciControllerStatePages{17U};
inline constexpr Foundation::UInt64 XhciStateFirstPages{32U};
inline constexpr Foundation::UInt64 XhciStatePages{70U};
inline constexpr Foundation::UInt64 XhciStateLastPages{XhciStatePages - XhciStateFirstPages};
inline constexpr Foundation::Size XhciFirstDevices{4U};
static_assert(XhciStateFirstPages + XhciStateLastPages == XhciStatePages);
inline constexpr Foundation::UInt64 XhciWindowPages{XhciRegisterWindowPages + XhciDmaPages + XhciStatePages};
/// An xHCI driver's word 37 (ComponentMountFlagsWord): its catalog index in
/// the low byte, and XhciReportProgressFlag when the starter prints the
/// enumeration diagnostics the driver then sends on its report Endpoint
/// (User/Component/UsbProgressFrame.hpp). Other starters receive none.
/// UsbHostGroupFlag makes the instance the driver of the USB host table's
/// controllers instead (its low byte is then zero).
inline constexpr Foundation::UInt64 XhciControllerIndexMask{0xFFU};
inline constexpr Foundation::UInt64 XhciReportProgressFlag{0x100U};
inline constexpr Foundation::UInt64 UsbHostGroupFlag{0x200U};
/// The USB host group driver's window, in pages from its start: one page of
/// registers for each of its (up to four) functions with a memory BAR, its
/// DMA pages (two for each controller: an EHCI or UHCI frame list, or an
/// OHCI HCCA, then the controller's queue heads, descriptors and bounce
/// buffer), and its CPU-only state, never mapped for a device.
inline constexpr Foundation::UInt64 UsbHostControllers{4U};
inline constexpr Foundation::UInt64 UsbHostRegisterPages{UsbHostControllers};
inline constexpr Foundation::UInt64 UsbHostDmaPages{2U * UsbHostControllers};
inline constexpr Foundation::UInt64 UsbHostStatePages{24U};
inline constexpr Foundation::UInt64 UsbHostWindowPages{UsbHostRegisterPages + UsbHostDmaPages + UsbHostStatePages};
static_assert(UsbHostWindowPages <= XhciWindowPages, "the group reuses the xHCI driver's window");
/// Role-specific aliases; the PS/2 component does not map a device window.
inline constexpr Foundation::Size ComponentPs2DataWord{30U};
inline constexpr Foundation::Size ComponentPs2CommandWord{32U};
inline constexpr Foundation::Size ComponentPs2KeyboardLineWord{38U};
inline constexpr Foundation::Size ComponentPs2MouseLineWord{40U};
inline constexpr Foundation::UInt64 ComponentInputFailedExitCode{0x0000'0000'C0DE'00F0ULL};
/// Input client's broker Invoke capability and epoch, reusing service words.
inline constexpr Foundation::Size ComponentInputServiceWord{34U};
inline constexpr Foundation::Size ComponentInputEpochWord{36U};
/// The network stack's window: its two receive pages, then its two transmit
/// pages, each a one-page Frame of its own (the NIC protocol maps one page
/// per request).
inline constexpr Foundation::UInt64 NetworkWindowPages{4U};

/// The NIC driver's window, in pages from its start: the first
/// NicRegisterPages pages of the device's register BAR (offsets below
/// 0x8000: the control, interrupt, receive and transmit registers and the
/// receive addresses), the descriptor page (16 receive descriptors at offset
/// 0, 16 transmit descriptors at offset 2048), the receive buffers (16 of
/// 2 KiB), the transmit buffers (8 of 2 KiB), and the page the driver maps
/// a client's Frame at while it copies. The DMA pages are Frames of the
/// driver's own domain (one of a page, one of NicReceiveBufferPages, one of
/// NicTransmitBufferPages), each mapped for the device at the address
/// IommuMap answers: the fixed one asked for below behind a translating
/// IOMMU (the VT-d backend keeps its table pages until the domain is
/// released), the physical one on the trusted-driver profile.
inline constexpr Foundation::UInt64 NicRegisterPages{8U};
inline constexpr Foundation::UInt64 NicDescriptorPage{8U};
inline constexpr Foundation::UInt64 NicReceiveBufferPage{9U};
inline constexpr Foundation::UInt64 NicReceiveBufferPages{8U};
inline constexpr Foundation::UInt64 NicTransmitBufferPage{17U};
inline constexpr Foundation::UInt64 NicTransmitBufferPages{4U};
/// A conservative sum of the boot's bounded private DMA buffers, even if every supported controller
/// uses 32-bit DMA. All xHCI controllers share XhciDmaPages and its scratchpad budget.
inline constexpr Foundation::UInt64 BootDmaLowPages{1U + AhciBouncePages + 1U + NicReceiveBufferPages +
                                                    NicTransmitBufferPages + UsbHostDmaPages + XhciDmaPages};
inline constexpr Foundation::UInt64 BootDmaLowBlocks{(BootDmaLowPages + 511U) / 512U};
static_assert(BootDmaLowBlocks == 1U);
inline constexpr Foundation::UInt64 NicClientPage{21U};
inline constexpr Foundation::UInt64 NicWindowPages{22U};
inline constexpr Foundation::UInt64 NicDescriptorDevice{0x0010'0000U};
inline constexpr Foundation::UInt64 NicReceiveBufferDevice{0x0020'0000U};
inline constexpr Foundation::UInt64 NicTransmitBufferDevice{0x0030'0000U};
static_assert((NicDescriptorPage == NicRegisterPages) && (NicReceiveBufferPage == (NicDescriptorPage + 1U)) &&
                  (NicTransmitBufferPage == (NicReceiveBufferPage + NicReceiveBufferPages)) &&
                  (NicClientPage == (NicTransmitBufferPage + NicTransmitBufferPages)) &&
                  (NicWindowPages == (NicClientPage + 1U)),
              "the NIC window's parts follow each other");

/// An interpreter's extension record, at byte 128 of the start-block page,
/// right after the 16 words: the magic and version, the address and size of
/// the foreign executable's bytes, mapped read-only, and its identity as the
/// loader's ExecutableFormat, ExecutableMachine, ExecutableAbi and
/// ExecutableByteOrder values (User/Loader/ExecutableImage.hpp). Other
/// components ignore it; an interpreter checks every access against the
/// size it gives.
inline constexpr Foundation::Size ComponentForeignMagicWord{16U};
inline constexpr Foundation::Size ComponentForeignVersionWord{17U};
inline constexpr Foundation::Size ComponentForeignAddressWord{18U};
inline constexpr Foundation::Size ComponentForeignBytesWord{19U};
inline constexpr Foundation::Size ComponentForeignFormatWord{20U};
inline constexpr Foundation::Size ComponentForeignMachineWord{21U};
inline constexpr Foundation::Size ComponentForeignAbiWord{22U};
inline constexpr Foundation::Size ComponentForeignByteOrderWord{23U};
inline constexpr Foundation::Size ComponentInterpreterBlockWords{24U};
inline constexpr Foundation::UInt64 ComponentForeignMagic{0x314E'4749'4552'4F46ULL};
inline constexpr Foundation::UInt64 ComponentForeignVersion{1U};
/// Where AMD64's root service maps a foreign executable in an interpreter's
/// space (clear of the component images at 0x40000000 and of the stack),
/// and the most pages it stages.
inline constexpr Foundation::UInt64 ComponentForeignImageAddress{0x6000'0000U};
inline constexpr Foundation::UInt64 ComponentForeignImagePages{16U};
static_assert(Ia64ComponentForeignImageAddress + (ComponentForeignImagePages * 4096U) <= Ia64ComponentSharedCodeStart);

/// What a component holds of its own domain, at words 24-29, after the interpreter's record; zero
/// where it gets none: a copy of its ResourceDomain's capability with Manage and Inspect only, a
/// copy of its AddressSpace's with Map only, and the start and page count of a window of its space
/// that nothing maps, which its domain's page limit covers besides its image.
inline constexpr Foundation::Size ComponentAccountWord{24U};
inline constexpr Foundation::Size ComponentAccountSpaceWord{26U};
inline constexpr Foundation::Size ComponentWindowStartWord{28U};
inline constexpr Foundation::Size ComponentWindowPagesWord{29U};
/// A driver's device, at words 30-33; zero otherwise: a copy of the device's DeviceRegion with Map,
/// Read and Write, and of its PCI function with Invoke. The display broker's scanout is a copy of
/// its DeviceRegion at word 30, with no function.
inline constexpr Foundation::Size ComponentDeviceRegionWord{30U};
inline constexpr Foundation::Size ComponentDeviceFunctionWord{32U};
/// A filesystem service's block service, at words 34-39; zero otherwise: a copy of a send
/// capability on the block driver's Endpoint (Invoke), the driver instance's tag it addresses, its
/// mount flags (ComponentMount*), and its own block-reply Endpoint (Read, Invoke and Duplicate),
/// which the root service bound to its WaitSet beside its service Endpoint.
inline constexpr Foundation::Size ComponentBlockServiceWord{34U};
inline constexpr Foundation::Size ComponentBlockGenerationWord{36U};
inline constexpr Foundation::Size ComponentMountFlagsWord{37U};
inline constexpr Foundation::Size ComponentBlockReplyWord{38U};
// filesystem-only manager receive capability.
inline constexpr Foundation::Size ComponentFsManagerWord{40U};
/// A mount flag: a blank volume (no FAT boot sector, both AsteriaFS superblock sectors zero) is
/// formatted as AsteriaFS; without it a blank volume is not mounted.
inline constexpr Foundation::UInt64 ComponentMountFormatBlank{1U};
/// A network stack's configuration, in the mount flags' word: its IPv4 address in bits 0-31
/// (a.b.c.d as a << 24 | b << 16 | c << 8 | d) and its prefix length in bits 32-39; on-link only.
inline constexpr Foundation::Size ComponentNetworkConfigurationWord{ComponentMountFlagsWord};
/// The display broker's scanout mode, at words 40 and 41, as the pager's start block packs it:
/// width | height << 32, then pixels per scan line | format << 32 (1 RGBX, 2 BGRX). The words a
/// component reads include its mapping address in word 42.
inline constexpr Foundation::Size ComponentScanoutModeWord{40U};
inline constexpr Foundation::Size ComponentScanoutAddressWord{42U};
/// The USB host group driver's further functions (UsbHostGroupFlag): its
/// first at words 30-33 as any driver's, the second to fourth at words 45-56,
/// each a DeviceRegion copy (Map, Read, Write) then a PciFunction copy
/// (Invoke); zero where the table has none.
inline constexpr Foundation::Size ComponentUsbHostDeviceWord{45U};
[[nodiscard]] constexpr Foundation::Size ComponentUsbHostRegionWord(const Foundation::Size controller) noexcept
{
    return controller == 0U ? ComponentDeviceRegionWord : ComponentUsbHostDeviceWord + ((controller - 1U) << 2U);
}
[[nodiscard]] constexpr Foundation::Size ComponentUsbHostFunctionWord(const Foundation::Size controller) noexcept
{
    return ComponentUsbHostRegionWord(controller) + 2U;
}
inline constexpr Foundation::Size ComponentBlockWords{57U};
static_assert(ComponentPs2DeclaredWord + 1U == ComponentUsbHostDeviceWord);
static_assert(ComponentUsbHostFunctionWord(UsbHostControllers - 1U) + 2U == ComponentBlockWords);
/// Where AMD64's display broker maps the scanout in its own space: clear of
/// the images, the stack, the foreign executable, the window and the
/// kernel's MMIO window, with room for a scanout of 32 MiB.
inline constexpr Foundation::UInt64 ComponentScanoutAddress{0x8000'0000U};
/// IA-64's scanout is in user region 1, above the component image limit,
/// Clear of its stack and window; no foreign image runs on IA-64.
inline constexpr Foundation::UInt64 Ia64ComponentScanoutAddress{0x2000'0000'8000'0000ULL};
/// AMD64's window, clear of the images at 0x40000000, the stack, the
/// Foreign executable and the kernel's MMIO window and scrub slot at 0x70000000, which every root
/// maps.
inline constexpr Foundation::UInt64 ComponentWindowStart{0x6800'0000U};
inline constexpr Foundation::UInt64 ComponentWindowPages{2U};
/// AMD64 counterpart of the IA-64 reservation: 256 MiB at 64 GiB, clear of the fixed image, stack,
/// foreign bytes, account/DMA windows, 4 MiB scanout and the 4 GiB firmware relocation preset,
/// below the canonical lower-half limit.
inline constexpr Foundation::UInt64 ComponentSharedCodeStart{0x0000'0010'0000'0000ULL};
inline constexpr Foundation::UInt64 ComponentSharedCodeLimit{0x0000'0010'1000'0000ULL};
static_assert(ComponentStackStart + ((ComponentStackPages + 1U) * 4096U) <= ComponentSharedCodeStart);
static_assert(ComponentForeignImageAddress + (ComponentForeignImagePages * 4096U) <= ComponentSharedCodeStart);
static_assert(ComponentWindowStart + (ComponentWindowPages * 4096U) <= ComponentSharedCodeStart);
static_assert(ComponentScanoutAddress + (32U * 1024U * 1024U) <= ComponentSharedCodeStart);
static_assert(ComponentSharedCodeStart < ComponentSharedCodeLimit);
static_assert(ComponentSharedCodeLimit < 0x0000'8000'0000'0000ULL);
/// Broker-only read-only output, after the scanout.
inline constexpr Foundation::UInt64 ComponentSessionOffsetBytes{32U * 1024U * 1024U};
inline constexpr Foundation::UInt64 ComponentSessionAddress{ComponentScanoutAddress + ComponentSessionOffsetBytes};
inline constexpr Foundation::UInt64 Ia64ComponentSessionAddress{Ia64ComponentScanoutAddress +
                                                                ComponentSessionOffsetBytes};
inline constexpr Foundation::UInt64 ComponentSessionMaximumPages{8192U};
static_assert(ComponentSessionAddress + ComponentSessionMaximumPages * 4096U <= ComponentSharedCodeStart);
static_assert(Ia64ComponentSharedCodeLimit <= Ia64ComponentScanoutAddress);
static_assert(Ia64ComponentSessionAddress + ComponentSessionMaximumPages * 4096U < 0x4000'0000'0000'0000ULL);
/// Fixed 1 GiB-aligned compositor region, clear of other fixed user ranges; uses a large arena.
inline constexpr Foundation::UInt64 ComponentCompositorAddress{0xC000'0000ULL};
inline constexpr Foundation::UInt64 Ia64ComponentCompositorAddress{0x2000'0000'C000'0000ULL};
static_assert(ComponentSessionAddress + ComponentSessionMaximumPages * 4096U <= ComponentCompositorAddress);
static_assert(ComponentCompositorAddress + 306U * 1024U * 1024U <= ComponentSharedCodeStart);
static_assert(Ia64ComponentSessionAddress + ComponentSessionMaximumPages * 4096U <= Ia64ComponentCompositorAddress);
static_assert(Ia64ComponentCompositorAddress + 306U * 1024U * 1024U < 0x4000'0000'0000'0000ULL);
/// Every AMD64 user range a component, image or process uses at a fixed address lies at or above
/// this one: the images at 0x40000000, the stack and start block, the foreign executable, the
/// window, the scanout and the shared-code reservation, besides the kernel's own MMIO window and
/// scrub slots at 0x70000000. The metadata arena, which every root maps at its identity address,
/// lies below it.
inline constexpr Foundation::UInt64 Amd64FixedUserRangesStart{0x4000'0000U};
static_assert(ComponentStackStart >= Amd64FixedUserRangesStart);
static_assert(ComponentForeignImageAddress >= Amd64FixedUserRangesStart);
static_assert(ComponentWindowStart >= Amd64FixedUserRangesStart);
static_assert(ComponentScanoutAddress >= Amd64FixedUserRangesStart);
static_assert(ComponentSharedCodeStart >= Amd64FixedUserRangesStart);

/// How a crashing incarnation crashes: by an instruction its ISA refuses, or by a floating-point
/// division by zero with that exception unmasked (AMD64 an x87 #MF, IA-64 a Floating-point Fault),
/// which the kernel hands to the supervisor as well.
inline constexpr Foundation::UInt64 ComponentCrashInvalidInstruction{0U};
inline constexpr Foundation::UInt64 ComponentCrashFloatingPoint{1U};

/// A reporting incarnation's exit code, after its report; the codes an
/// Incarnation exits with otherwise: a start block it does not recognize, a refused report, a crash
/// that returned.
inline constexpr Foundation::UInt64 ComponentExitCode{0x0000'0000'C0DE'0002ULL};
inline constexpr Foundation::UInt64 ComponentBadStartExitCode{0x0000'0000'C0DE'00E1ULL};
inline constexpr Foundation::UInt64 ComponentReportFailedExitCode{0x0000'0000'C0DE'00E2ULL};
inline constexpr Foundation::UInt64 ComponentCrashReturnedExitCode{0x0000'0000'C0DE'00E3ULL};
/// An echo incarnation's wait, receive or reply failed.
inline constexpr Foundation::UInt64 ComponentEchoFailedExitCode{0x0000'0000'C0DE'00E4ULL};
/// A reporting incarnation's floating-point or integer-division result was wrong after the
/// Computation was preempted.
inline constexpr Foundation::UInt64 ComponentArithmeticFailedExitCode{0x0000'0000'C0DE'00E5ULL};
/// An interpreter's exit code after its report, and when the launch was not one it could take.
inline constexpr Foundation::UInt64 ComponentInterpretedExitCode{0x0000'0000'C0DE'0007ULL};
inline constexpr Foundation::UInt64 ComponentInterpretFailedExitCode{0x0000'0000'C0DE'00E7ULL};
/// An account incarnation's exit code after its report, and when a step was
/// not as its domain requires: the step in bits 32-39 (the Endpoint, the
/// Frame, the MemoryObject, the mapping, the memory read back, the refusal
/// past its pages, the executable-memory refusal) and the error the kernel answered in bits 40-47.
inline constexpr Foundation::UInt64 ComponentAccountExitCode{0x0000'0000'C0DE'0008ULL};
inline constexpr Foundation::UInt64 ComponentAccountFailedExitCode{0x0000'0000'C0DE'00E8ULL};
inline constexpr Foundation::UInt64 ComponentAccountEndpointStep{1U};
inline constexpr Foundation::UInt64 ComponentAccountFrameStep{2U};
inline constexpr Foundation::UInt64 ComponentAccountMemoryObjectStep{3U};
inline constexpr Foundation::UInt64 ComponentAccountMapStep{4U};
inline constexpr Foundation::UInt64 ComponentAccountMemoryStep{5U};
inline constexpr Foundation::UInt64 ComponentAccountRefusalStep{6U};
inline constexpr Foundation::UInt64 ComponentAccountCodeRefusalStep{7U};
/// A driver's exit code after its report, and when a step was not as the
/// device requires: the step in bits 32-39 (the device Frame, the
/// MemoryObject, the mapping, the identification, the liveness check, the
/// factorial; before them, bus mastering refused without an IommuDomain and
/// the assignee's own disable; after them, the IommuDomain, the attachment,
/// the two pages, their device mappings, bus mastering, the DMA through
/// them, the unmap, the stray write that must fault and not land, the DMA
/// after mapping again, and the release; with bus mastering on, the WaitSet
/// and Interrupt for its messages, their binding and arming, and the
/// interrupt the driver raises, which must wake it; a driver service's
/// quiesce of what a dead instance left running, its serving, and a device
/// that no longer behaves) and the kernel's error in bits 40-47.
inline constexpr Foundation::UInt64 ComponentDriverExitCode{0x0000'0000'C0DE'0009ULL};
inline constexpr Foundation::UInt64 ComponentDriverFailedExitCode{0x0000'0000'C0DE'00E9ULL};
/// The exit code of a driver on a kernel without an IOMMU: it drove its device through its
/// registers, found bus mastering refused and IommuDomain creation not configured, and so did no
/// DMA.
inline constexpr Foundation::UInt64 ComponentDriverRegistersExitCode{0x0000'0000'C0DE'000AULL};
inline constexpr Foundation::UInt64 ComponentDriverFrameStep{1U};
inline constexpr Foundation::UInt64 ComponentDriverMemoryObjectStep{2U};
inline constexpr Foundation::UInt64 ComponentDriverMapStep{3U};
inline constexpr Foundation::UInt64 ComponentDriverIdentityStep{4U};
inline constexpr Foundation::UInt64 ComponentDriverLivenessStep{5U};
inline constexpr Foundation::UInt64 ComponentDriverFactorialStep{6U};
inline constexpr Foundation::UInt64 ComponentDriverBusMasterStep{7U};
inline constexpr Foundation::UInt64 ComponentDriverDisableStep{8U};
inline constexpr Foundation::UInt64 ComponentDriverIommuStep{9U};
inline constexpr Foundation::UInt64 ComponentDriverAttachStep{10U};
inline constexpr Foundation::UInt64 ComponentDriverPagesStep{11U};
inline constexpr Foundation::UInt64 ComponentDriverDeviceMapStep{12U};
inline constexpr Foundation::UInt64 ComponentDriverMasterStep{13U};
inline constexpr Foundation::UInt64 ComponentDriverDmaStep{14U};
inline constexpr Foundation::UInt64 ComponentDriverUnmapStep{15U};
inline constexpr Foundation::UInt64 ComponentDriverStrayStep{16U};
inline constexpr Foundation::UInt64 ComponentDriverRemapStep{17U};
inline constexpr Foundation::UInt64 ComponentDriverReleaseStep{18U};
inline constexpr Foundation::UInt64 ComponentDriverInterruptStep{19U};
inline constexpr Foundation::UInt64 ComponentDriverRaiseStep{20U};
inline constexpr Foundation::UInt64 ComponentDriverQuiesceStep{21U};
inline constexpr Foundation::UInt64 ComponentDriverServeStep{22U};
inline constexpr Foundation::UInt64 ComponentDriverDeviceStep{23U};
inline constexpr Foundation::UInt64 ComponentDriverScratchpadStep{24U};
inline constexpr Foundation::UInt64 ComponentDriverPageSizeStep{25U};
/// The display broker's exit code when a step was not as its scanout
/// Requires: the step in bits 32-39 (a mode it cannot draw, the device Frame, the MemoryObject, the
/// mapping, its wait, its receive) and the kernel's error in bits 40-47. It never exits otherwise:
/// it serves until killed.
inline constexpr Foundation::UInt64 ComponentDisplayFailedExitCode{0x0000'0000'C0DE'00EDULL};
inline constexpr Foundation::UInt64 ComponentDisplayModeStep{1U};
inline constexpr Foundation::UInt64 ComponentDisplayFrameStep{2U};
inline constexpr Foundation::UInt64 ComponentDisplayMemoryObjectStep{3U};
inline constexpr Foundation::UInt64 ComponentDisplayMapStep{4U};
inline constexpr Foundation::UInt64 ComponentDisplayWaitStep{5U};
inline constexpr Foundation::UInt64 ComponentDisplayReceiveStep{6U};
/// The filesystem service's exit code when a step of its own failed: the
/// Step in bits 32-39 (a window of fewer than two pages, its bounce Frame, a MemoryObject, the
/// bounce page's mapping, its wait, a receive on its service or block-reply Endpoint, the unmap of
/// a client's Frame, the close of a capability other than one already gone) and the kernel's error
/// in bits 40-47. Device failures and client errors are answered, never an exit; a crash a Commit
/// injects that returned exits with ComponentCrashReturnedExitCode.
inline constexpr Foundation::UInt64 ComponentFilesystemFailedExitCode{0x0000'0000'C0DE'00EEULL};
inline constexpr Foundation::UInt64 ComponentFilesystemWindowStep{1U};
inline constexpr Foundation::UInt64 ComponentFilesystemFrameStep{2U};
inline constexpr Foundation::UInt64 ComponentFilesystemMemoryObjectStep{3U};
inline constexpr Foundation::UInt64 ComponentFilesystemMapStep{4U};
inline constexpr Foundation::UInt64 ComponentFilesystemWaitStep{5U};
inline constexpr Foundation::UInt64 ComponentFilesystemReceiveStep{6U};
inline constexpr Foundation::UInt64 ComponentFilesystemUnmapStep{7U};
inline constexpr Foundation::UInt64 ComponentFilesystemCloseStep{8U};
/// The network stack service's exit code: the step in bits 32-39 and the kernel's error in bits
/// 40-47, numbered as the filesystem service's where they mean the same: a window of fewer than
/// Four pages, one of its four Frames, a MemoryObject, a page's mapping, its wait, a receive on its
/// service or driver-reply Endpoint, its configuration (word 37 without a prefix length of 1 to 32,
/// or an address or MAC address the stack refuses), the close of a capability other than one
/// already gone, the driver's Query at start (answered other than Done, or none within the
/// startup timeout: TimedOut), and the driver itself: a NicDriverRestarted reply (no error), or a
/// request the kernel refused to pass to it with ObjectNotFound, InvalidHandle or Revoked (its
/// Endpoint is gone), or a receive credit refused otherwise (that error). Rules, bindings and
/// outstanding Receives end with the instance; the root service restarts the stack after its
/// driver. Client errors are answered, never an exit.
inline constexpr Foundation::UInt64 ComponentNetworkFailedExitCode{0x0000'0000'C0DE'00EFULL};
inline constexpr Foundation::UInt64 ComponentNetworkWindowStep{1U};
inline constexpr Foundation::UInt64 ComponentNetworkFrameStep{2U};
inline constexpr Foundation::UInt64 ComponentNetworkMemoryObjectStep{3U};
inline constexpr Foundation::UInt64 ComponentNetworkMapStep{4U};
inline constexpr Foundation::UInt64 ComponentNetworkWaitStep{5U};
inline constexpr Foundation::UInt64 ComponentNetworkReceiveStep{6U};
inline constexpr Foundation::UInt64 ComponentNetworkConfigurationStep{7U};
inline constexpr Foundation::UInt64 ComponentNetworkCloseStep{8U};
inline constexpr Foundation::UInt64 ComponentNetworkQueryStep{9U};
inline constexpr Foundation::UInt64 ComponentNetworkDriverStep{10U};
} // Namespace Asteria::Abi

#endif // ASTERIA_ABI_COMPONENT_BOOTSTRAP_HPP_INCLUDED
