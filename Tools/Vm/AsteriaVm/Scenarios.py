# SPDX-License-Identifier: Apache-2.0
"""The VM test cases and the suites that select them.

Every case names the CMake preset whose firmware it boots, so each variant
(relocated image base, diagnostic probes) has its own build tree and no case
rebuilds or restores firmware for another. Every tree builds two firmwares
from the same kernel objects: the test firmware, whose initial user image is
the guest test program (its TAP stream carries the verdicts on everything
the system-call ABI shows), and the production firmware. The default cases
boot the test firmware; one smoke case per ISA boots the production
firmware, and so do the diagnostic probes, which run their own kernel-side
scenarios.
"""
from __future__ import annotations

import dataclasses

# The manifest variants (AsteriaVerify.json) of a tree's two firmwares.
TEST_FIRMWARE = 'firmware-test'
PRODUCTION_FIRMWARE = 'firmware'


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    isa: str
    preset: str
    cpus: int
    disk: str                       # '', raw, fat12, fat16, fat32 or exfat; truthy means attached
    driver: str
    relocated: bool = False
    suites: frozenset[str] = frozenset()
    expected_stop: str | None = None
    expected_online_mask: int | None = None
    reaches_userspace: bool = True
    firmware: str = TEST_FIRMWARE      # The variant whose `esp`, `user_elf` and `kernel_elf` the case uses
    boot_services_absent: bool = False
    xhci: bool = False
    usb_boot: bool = False           # Writable MBR ESP on this xHCI controller
    xhci_failure: bool = False       # Intentionally omit DMA isolation
    xhci_large: bool = False         # 15 USB2 + 15 USB3 ports, HID on logical ports 14/15
    i8042: bool = True
    fadt8042: bool = True
    floating_keyboard: bool = False
    product_media: bool = False
    x2apic: bool = False
    pit: bool = True
    aml: str = ''                    # Facts or skip: writable ESP checks
    edid: tuple[int, int] | None = None
    graphics_device: str = 'VGA'
    display_policy: str = ''
    expected_display: tuple[int, int] | None = None
    iommu: str = 'Default'             # VT-d on AMD64, virtio-iommu on IA-64; or AmdVi, VtD (IA-64), or None
    # IA-64 only: a machine of the VT-d branch build (IA64_VTD_QEMU and
    # IA64_VTD_FIRMWARE, Documentation/Architecture/IommuDomain.md ), with
    # extra -machine properties, or hp-i2000 of the default build. Empty keeps
    # the default itanium2-vpc of IA64_QEMU.
    machine: str = ''
    machine_options: str = ''
    # The trusted-driver profile : the drivers ('ahci', 'xhci', 'e1000e') whose functions get
    # BOOT.CFG trust entries (AsteriaVm.Trust), and the test firmware's assignable devices the
    # machine attaches (edu, whose demonstration needs a translating IOMMU, and the e1000e, whose
    # network demonstration needs an IOMMU or a trust entry).
    trust: tuple[str,...] = ()
    assignable: tuple[str,...] = ('edu', 'e1000e')
    # The USB host group driver's controllers (Machine.USB_HOST_CONTROLLERS:
    # 'ich9' or, IA-64 only, 'nec') and where the keyboard and the mouse sit
    # (Machine.USB_HOST_LAYOUTS); empty attaches none.
    usb_host: str = ''
    usb_layout: str = 'split'
    # AMD64: two Q35 ICH9 EHCIs reserved by a synthetic RMRR, without an
    # IOMMU device (AsteriaVm.Dmar).
    synthetic_rmrr: bool = False
    fault_channel: bool = False       # Independent created supervisor
    memory_rmrr: bool = False
    memory: str = ''                 # Explicit RAM size for import evidence
    ui_font_absent: bool = False
    power: str = ''                # Off or reboot; clean product terminal path
    toolkit_session: bool = False
    session_compositor: bool = False
    session_input: bool = False
    session_large: bool = False
    tcg_thread: str = 'multi'  # AMD64; IA-64 always uses single
    whole_machine_pause: str = ''  # Early or session; SIGSTOP leaves the virtual clock running
    product_desktop: bool = False
    session_face_withheld: bool = False
    session_compositor_fault: bool = False

    session_stress: bool = False

    login_failure: str = ''
    session_probe: bool = False       # Desktop session, root-started test module

    @property
    def usb_console(self) -> bool:
        """Whether QMP names the `usbconsole` display so USB input reaches
        the USB keyboard rather than a PS/2 one: AMD64's USB cases (an
        IA-64 USB host case has no PS/2 keyboard, so QMP's default reaches
        the USB devices)."""
        return (self.xhci and not self.xhci_failure) or (self.usb_host != '' and self.isa == 'Amd64')

    @property
    def has_disk(self) -> bool:
        return bool(self.disk)

    @property
    def guest(self) -> bool:
        """Whether the guest test program runs (and reports TAP) in this case."""
        return self.firmware == TEST_FIRMWARE

    @property
    def weight(self) -> int:
        """Host threads the case keeps busy: IA-64 runs single-threaded TCG,
        AMD64 multi-threaded TCG uses up to one thread per vCPU."""
        return 1 if self.isa == 'Ia64' else min(self.cpus, 2)


def _ia64_matrix() -> list[Case]:
    cases = [Case('Ia64.Smp%d' % cpus, 'Ia64', 'ia64', cpus, '', 'Ia64Default', i8042=False,
                  suites=frozenset({'All', 'Ia64'} | ({'Ia64Ahci'} if cpus == 1 else set())))
             for cpus in (4, 3, 2, 1)]
    cases.append(Case('Ia64.Ahci', 'Ia64', 'ia64', 1, 'raw', 'Ia64Default', i8042=False,
                      suites=frozenset({'All', 'Ia64', 'Ia64Ahci'})))
    # The filesystem service's other formats behind the AHCI driver
    # as AMD64's: FAT12, FAT16 (its readme asks for
    # the crash before the directory entry), FAT32 and exFAT.
    cases += [Case('Ia64.%s' % name, 'Ia64', 'ia64', 1, recipe, 'Ia64Default', i8042=False,
                   suites=frozenset({'All', 'Ia64', 'Ia64Ahci'}))
              for name, recipe in (('Fat12', 'fat12'), ('Fat16', 'fat16'), ('Fat32', 'fat32'), ('Exfat', 'exfat'))]
    cases.append(Case('Ia64.Relocated', 'Ia64', 'ia64-relocated', 4, '', 'Ia64Default', i8042=False, relocated=True,
                      suites=frozenset({'All', 'Ia64'})))
    cases.append(Case('Ia64.Production', 'Ia64', 'ia64', 1, '', 'Production', suites=frozenset({'All', 'Ia64'}),
                      firmware=PRODUCTION_FIRMWARE))
    # Without an IOMMU edu drives its registers only; the e1000e, whose
    # driver could serve nothing, stays detached (as on AMD64, Source/User/Network/NetworkStack.hpp
    # ).
    cases.append(Case('Ia64.NoIommu', 'Ia64', 'ia64', 1, '', 'Ia64Default', i8042=False,
                      suites=frozenset({'All', 'Ia64'}), iommu='None', assignable=('edu',)))
    # Itanium2-vpc,i8042=on with the FADT bit masked: the namespace keyboard publishes. Every other
    # IA-64 default case runs i8042=off and checks that nothing is published and boot continues
    # (I8042Evidence).
    cases.append(Case('Ia64.Ps2', 'Ia64', 'ia64', 1, '', 'Ia64Default',
                      suites=frozenset({'All', 'Ia64'}), i8042=True, aml='facts', fadt8042=False))
    return cases


def _amd64_matrix(prefix: str, preset: str, relocated_preset: str, driver: str, suites: set[str],
                  firmware: str) -> list[Case]:
    cases = [Case('%s.Smp%d' % (prefix, cpus), 'Amd64', preset, cpus, '', driver, suites=frozenset(suites),
                  firmware=firmware)
             for cpus in (4, 1, 2, 3)]
    cases.append(Case('%s.Ahci' % prefix, 'Amd64', preset, 1, 'raw', driver, suites=frozenset(suites),
                      firmware=firmware))
    cases.append(Case('%s.Relocated' % prefix, 'Amd64', relocated_preset, 4, '', driver, relocated=True,
                      suites=frozenset(suites), firmware=firmware))
    return cases


CASES: tuple[Case,...] = tuple(
    _ia64_matrix()
    + _amd64_matrix('Amd64', 'amd64', 'amd64-relocated', 'Amd64Default', {'All', 'Amd64'}, TEST_FIRMWARE)
    + [Case('Amd64.Fat%s' % kind, 'Amd64', 'amd64', 1, 'fat' + kind, 'Amd64Default',
            suites=frozenset({'All', 'Amd64'})) for kind in ('12', '16', '32')]
    + [Case('Amd64.Exfat', 'Amd64', 'amd64', 1, 'exfat', 'Amd64Default',
            suites=frozenset({'All', 'Amd64'}))]
    + [Case('Amd64.BootServicesAbsent', 'Amd64', 'amd64', 1, '', 'Amd64Default',
            suites=frozenset({'All', 'Amd64'}), boot_services_absent=True)]
    + [Case('Amd64.Production', 'Amd64', 'amd64', 1, '', 'Production', suites=frozenset({'All', 'Amd64'}),
            firmware=PRODUCTION_FIRMWARE)]
    + [Case('Amd64.NoI8042', 'Amd64', 'amd64', 1, '', 'Amd64Default', i8042=False,
            suites=frozenset({'Amd64'}))]
    + _amd64_matrix('Amd64Preemption', 'amd64-preemption-probe', 'amd64-preemption-probe-relocated',
                    'Amd64NativePreemption', {'Amd64NativePreemption'}, PRODUCTION_FIRMWARE)
    + [Case('Ia64.SyntheticProbe', 'Ia64', 'ia64-synthetic-probe', 1, '', 'Ia64SyntheticProbe', i8042=False,
            suites=frozenset({'Ia64SyntheticProbe'}), firmware=PRODUCTION_FIRMWARE),
       Case('Ia64.NativePreemption', 'Ia64', 'ia64-preemption-probe', 1, '', 'Ia64NativePreemption', i8042=False,
            suites=frozenset({'Ia64NativePreemption'}), firmware=PRODUCTION_FIRMWARE)])

CASES += (Case('Amd64X2Apic.Smp4', 'Amd64', 'amd64-x2apic-probe', 4, '', 'Amd64Default',
               suites=frozenset({'Amd64X2Apic'}), x2apic=True),
          Case('Amd64X2Apic.Nmi', 'Amd64', 'amd64-x2apic-probe', 4, '', 'Amd64Stop',
               suites=frozenset({'Amd64X2ApicStops'}), expected_stop='UnexpectedNmi', x2apic=True),
          Case('Amd64NoPit.Smp4', 'Amd64', 'amd64', 4, '', 'Amd64Default',
               suites=frozenset({'Amd64NoPit'}), pit=False))

CASES += tuple(Case('%s.%s' % (isa, name), isa, isa.lower(), 1, '', isa + 'Default',
                    suites=frozenset({'All', isa}), aml=mode, i8042=isa == 'Amd64')
               for isa in ('Amd64', 'Ia64') for name, mode in (('Aml', 'facts'), ('NoAml', 'skip')))

CASES += (Case('Amd64.AmdVi', 'Amd64', 'amd64', 1, 'raw', 'Amd64Default',
               suites=frozenset({'Amd64AmdVi'}), iommu='AmdVi'),)

CASES += (Case('Amd64Stop.Injected', 'Amd64', 'amd64-stop-probe', 4, '', 'Amd64Stop',
               suites=frozenset({'Amd64Stops'}), expected_stop='TestInjected'),
          Case('Amd64Stop.Nmi', 'Amd64', 'amd64', 4, '', 'Amd64Stop',
               suites=frozenset({'Amd64Stops'}), expected_stop='UnexpectedNmi'),
          Case('Amd64Stop.Early', 'Amd64', 'amd64-stop-early-probe', 4, '', 'Amd64Stop',
               suites=frozenset({'Amd64StopEarly'}), expected_stop='TestInjected', expected_online_mask=1, reaches_userspace=False))

# IA-64 behind Intel VT-d on the 7500-class hub of `itanium2-vpc-iommu` : edu's confined DMA with
# its stray write recorded by the unit's fault registers, the same with ECAP.C set (no table
# flushes) and with both register sets above 2^48; the AHCI driver and the filesystem service behind
# the same unit, on AsteriaFS (with its crashes) and on FAT16 (the crash before the directory
# entry); and the absence case: the branch build's plain itanium2-vpc has no DMAR and no
# virtio-iommu, so the boot chooses no IOMMU and edu stays registers-only (the e1000e detached, as
# Ia64.NoIommu's). Every case with an IOMMU runs the network demonstration on the e1000e behind the
# INCLUDE_PCI_ALL unit.
CASES += (Case('Ia64Vtd.Edu', 'Ia64', 'ia64', 1, '', 'Ia64Default', i8042=False, suites=frozenset({'Ia64Vtd'}),
               iommu='VtD', machine='itanium2-vpc-iommu'),
          Case('Ia64Vtd.Coherent', 'Ia64', 'ia64', 1, '', 'Ia64Default', i8042=False, suites=frozenset({'Ia64Vtd'}),
               iommu='VtD', machine='itanium2-vpc-iommu', machine_options='vtd-coherent=on'),
          Case('Ia64Vtd.HighMmio', 'Ia64', 'ia64', 1, '', 'Ia64Default', i8042=False, suites=frozenset({'Ia64Vtd'}),
               iommu='VtD', machine='itanium2-vpc-iommu', machine_options='vtd-high-mmio=on'),
          Case('Ia64Vtd.Ahci', 'Ia64', 'ia64', 1, 'raw', 'Ia64Default', i8042=False, suites=frozenset({'Ia64Vtd'}),
               iommu='VtD', machine='itanium2-vpc-iommu'),
          Case('Ia64Vtd.Fat16', 'Ia64', 'ia64', 1, 'fat16', 'Ia64Default', i8042=False, suites=frozenset({'Ia64Vtd'}),
               iommu='VtD', machine='itanium2-vpc-iommu'),
          Case('Ia64Vtd.Absent', 'Ia64', 'ia64', 1, '', 'Ia64Default', i8042=False, suites=frozenset({'Ia64Vtd'}),
               iommu='None', machine='itanium2-vpc', assignable=('edu',)))

CASES += (Case('Ia64Stop.Injected', 'Ia64', 'ia64-stop-probe', 4, '', 'Ia64Stop',
               suites=frozenset({'Ia64Stops'}), expected_stop='TestInjected'),
          Case('Ia64Stop.Early', 'Ia64', 'ia64-stop-early-probe', 4, '', 'Ia64Stop',
               suites=frozenset({'Ia64StopEarly'}), expected_stop='TestInjected',
               expected_online_mask=1, reaches_userspace=False))

CASES += (Case('Amd64.XhciHid', 'Amd64', 'amd64', 1, '', 'Xhci',
               suites=frozenset({'Amd64XhciHid'}), i8042=False, xhci=True),
          Case('Amd64.XhciProduct', 'Amd64', 'amd64', 1, '', 'Production',
               suites=frozenset({'Amd64XhciHid'}), firmware=PRODUCTION_FIRMWARE, xhci=True),
          Case('Amd64.XhciAmdVi', 'Amd64', 'amd64', 1, '', 'Xhci',
               suites=frozenset({'Amd64XhciHid'}), i8042=False, xhci=True, iommu='AmdVi'))

CASES += (Case('Amd64.XhciUsbProduct', 'Amd64', 'amd64', 2, '', 'Production',
               suites=frozenset({'Amd64XhciHid'}), firmware=PRODUCTION_FIRMWARE, xhci=True, usb_boot=True, edid=(1366, 768), graphics_device='bochs-display',
               expected_display=(1366, 768)),
          Case('Amd64.XhciUsbHardware', 'Amd64', 'amd64', 2, '', 'XhciHardware',
               suites=frozenset({'Amd64XhciHid'}), xhci=True, usb_boot=True),
          Case('Amd64.XhciFailedProduct', 'Amd64', 'amd64', 2, '', 'Production',
               suites=frozenset({'Amd64XhciHid'}), firmware=PRODUCTION_FIRMWARE,
               xhci=True, xhci_failure=True))

CASES += (Case('Amd64.AmlI8042', 'Amd64', 'amd64', 1, '', 'Amd64Default',
               suites=frozenset({'Amd64'}), fadt8042=False),
          Case('Amd64.FloatingI8042', 'Amd64', 'amd64', 1, '', 'Amd64Default',
               suites=frozenset({'Amd64'}), i8042=False, floating_keyboard=True),
          Case('Amd64.AmlProductMedia', 'Amd64', 'amd64', 1, '', 'Production',
               suites=frozenset({'Amd64'}), fadt8042=False, product_media=True,
               firmware=PRODUCTION_FIRMWARE))
CASES += (Case('Amd64.XhciUsbLargeProduct', 'Amd64', 'amd64', 2, '', 'Production',
               suites=frozenset({'Amd64XhciHid'}), firmware=PRODUCTION_FIRMWARE,
               xhci=True, usb_boot=True, xhci_large=True),
          Case('Amd64.XhciX2Apic', 'Amd64', 'amd64', 2, '', 'Xhci',
               suites=frozenset({'Amd64XhciHid'}), i8042=False, xhci=True, x2apic=True))

# QEMU's HP i2000 (Merced, 460GX, two-way) boots the production firmware to
# the root service with the diagnostic channel on COM1, which its SPCR names.
CASES += (Case('Ia64I2000.Production', 'Ia64', 'ia64', 1, '', 'Ia64I2000', suites=frozenset({'Ia64I2000'}),
               firmware=PRODUCTION_FIRMWARE, machine='hp-i2000', iommu='None'),
          Case('Ia64I2000.Smp2', 'Ia64', 'ia64', 2, '', 'Ia64I2000', suites=frozenset({'Ia64I2000'}),
               firmware=PRODUCTION_FIRMWARE, machine='hp-i2000', iommu='None'))

# The trusted-driver profile. On q35 without an IOMMU: the production console with its xHCI input
# and the AHCI controller trusted; the same machine without entries, where the xHCI driver fails; the entries with VT-d working, unused; the test firmware's storage and network scenarios
# with the AHCI controller and the e1000e trusted and edu left out (its demonstration needs a
# translating IOMMU), the AHCI driver's crash with a read in flight included; and its xHCI HID kill
# and reassignment with the xHCI trusted. On itanium2-vpc without virtio-iommu: the AHCI controller
# and the e1000e trusted under the filesystem service, on AsteriaFS and on FAT16, and the network
# stack with the LAN script; with virtio-iommu, the entries unused.
CASES += (Case('Amd64Trusted.Product', 'Amd64', 'amd64', 1, '', 'Trusted', suites=frozenset({'Amd64Trusted'}),
               firmware=PRODUCTION_FIRMWARE, xhci=True, iommu='None', trust=('ahci', 'xhci')),
          Case('Amd64Trusted.Untrusted', 'Amd64', 'amd64', 2, '', 'Trusted', suites=frozenset({'Amd64Trusted'}),
               firmware=PRODUCTION_FIRMWARE, xhci=True, xhci_failure=True),
          Case('Amd64Trusted.Vtd', 'Amd64', 'amd64', 1, '', 'Trusted', suites=frozenset({'Amd64Trusted'}),
               firmware=PRODUCTION_FIRMWARE, xhci=True, trust=('ahci', 'xhci')),
          Case('Amd64Trusted.Storage', 'Amd64', 'amd64', 1, 'raw', 'Trusted', suites=frozenset({'Amd64Trusted'}),
               i8042=False, iommu='None', trust=('ahci', 'e1000e'), assignable=('e1000e',)),
          Case('Amd64Trusted.Xhci', 'Amd64', 'amd64', 1, '', 'Trusted', suites=frozenset({'Amd64Trusted'}),
               i8042=False, xhci=True, iommu='None', trust=('xhci',), assignable=()),
          Case('Ia64Trusted.Ahci', 'Ia64', 'ia64', 1, 'raw', 'Trusted', i8042=False,
               suites=frozenset({'Ia64Trusted'}), iommu='None', trust=('ahci', 'e1000e')),
          Case('Ia64Trusted.Fat16', 'Ia64', 'ia64', 1, 'fat16', 'Trusted', i8042=False,
               suites=frozenset({'Ia64Trusted'}), iommu='None', trust=('ahci', 'e1000e')),
          Case('Ia64Trusted.Unused', 'Ia64', 'ia64', 1, '', 'Trusted', i8042=False,
               suites=frozenset({'Ia64Trusted'}), trust=('ahci', 'e1000e')))

# The USB host group driver on
# the product firmware: q35's ICH9 EHCI and UHCI companions behind VT-d, with
# PS/2 resident beside it (display, broker, PS/2 and the driver fill the
# component slots), as the high-speed keyboard on the EHCI and the
# full-speed mouse released to a companion, both full speed on one UHCI
# (the driver time-slices their polls), and both high speed on the EHCI;
# the split again behind AMD-Vi and under the trusted-driver profile without
# an IOMMU. IA-64's itanium2-vpc, without an i8042 so QMP's input reaches
# the USB devices, behind virtio-iommu: the zx-family NEC EHCI with an OHCI
# companion, and the rx2800's ICH9 EHCI with a UHCI companion, keyboard and
# mouse swapped.
CASES += (Case('Amd64.UsbHostProduct', 'Amd64', 'amd64', 1, '', 'Production', suites=frozenset({'Amd64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, usb_host='ich9', usb_layout='split'),
          Case('Amd64.UsbHostCompanion', 'Amd64', 'amd64', 1, '', 'Production', suites=frozenset({'Amd64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, usb_host='ich9', usb_layout='companion'),
          Case('Amd64.UsbHostEhci', 'Amd64', 'amd64', 1, '', 'Production', suites=frozenset({'Amd64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, usb_host='ich9', usb_layout='ehci'),
          Case('Amd64.UsbHostAmdVi', 'Amd64', 'amd64', 1, '', 'Production', suites=frozenset({'Amd64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, usb_host='ich9', usb_layout='split', iommu='AmdVi'),
          Case('Amd64Trusted.UsbHost', 'Amd64', 'amd64', 1, '', 'Trusted', suites=frozenset({'Amd64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, usb_host='ich9', usb_layout='split', iommu='None', trust=('usb',)),
          # Firmware RMRR diagnostics, kernel quarantine and the product console.
          Case('Amd64.EhciRmrrEvidence', 'Amd64', 'amd64', 1, '', 'EhciEvidence',
               suites=frozenset({'Amd64UsbHid'}), firmware=PRODUCTION_FIRMWARE, synthetic_rmrr=True),
          Case('Ia64.UsbHostNec', 'Ia64', 'ia64', 1, '', 'Production', suites=frozenset({'Ia64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, i8042=False, usb_host='nec', usb_layout='split'),
          Case('Ia64.UsbHostIch9', 'Ia64', 'ia64', 1, '', 'Production', suites=frozenset({'Ia64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, i8042=False, usb_host='ich9', usb_layout='swap'))

# The fifth component slot and the Device Manager's recovery of USB drivers : the product beside an
# xHCI (its keyboard and mouse there) with the ICH9's controllers and PS/2, display, broker, PS/2,
# xHCI and the USB host group all running; and the test firmware's recovery of the group, killed
# four times with its interrupt polls queued, restarted three times (typing and the pointer resume)
# and given up, on both ISAs (Drivers.UsbRecovery).
CASES += (Case('Amd64.UsbHostBesideXhci', 'Amd64', 'amd64', 1, '', 'Production', suites=frozenset({'Amd64UsbHid'}),
               firmware=PRODUCTION_FIRMWARE, xhci=True, usb_host='ich9', usb_layout='none'),
          Case('Amd64.UsbHostRecovery', 'Amd64', 'amd64', 1, '', 'UsbRecovery', suites=frozenset({'Amd64UsbHid'}),
               i8042=False, usb_host='ich9', usb_layout='split', assignable=()),
          Case('Ia64.UsbHostRecovery', 'Ia64', 'ia64', 1, '', 'UsbRecovery', suites=frozenset({'Ia64UsbHid'}),
               i8042=False, usb_host='nec', usb_layout='split'))

SUITES = ('ToolkitSession', 'Login', 'Amd64Login', 'Ia64Login', 'Power', 'Amd64Power', 'Ia64Power', 'FsV2', 'Amd64Watchdog', 'Ia64Watchdog', 'Amd64Session', 'Ia64Session', 'Amd64XhciHid', 'Amd64UsbHid', 'Ia64UsbHid', 'All', 'Ia64', 'Ia64Ahci', 'Ia64I2000', 'Ia64SyntheticProbe',
          'Ia64NativePreemption',
          'Ia64Vtd', 'Amd64', 'Amd64AmdVi', 'Amd64NativePreemption', 'Amd64Stops', 'Amd64StopEarly', 'Amd64X2Apic',
          'Amd64X2ApicStops', 'Amd64NoPit', 'Ia64Stops', 'Ia64StopEarly', 'Amd64Trusted', 'Ia64Trusted')


CASES += tuple(Case('%s.FaultChannel' % isa, isa, preset, 1, '', 'FaultChannel',
                    i8042=False, fault_channel=True, assignable=(), suites=frozenset({'All', isa}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')))

CASES += tuple(Case('%s.SessionProbe' % isa, isa, preset, 4 if isa == 'Amd64' else 1, '', 'SessionProbe',
                    i8042=False, session_probe=True, assignable=(),
                    suites=frozenset({'All', isa, isa + 'Session'}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')))

CASES += tuple(Case('%s.SessionCompositor' % isa, isa, preset, 4 if isa == 'Amd64' else 1, '', 'SessionCompositor',
                    i8042=False, session_compositor=True, assignable=(),
                    suites=frozenset({'All', isa, isa + 'Session'}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')))

CASES += tuple(Case('%s.SessionFontFallback' % isa, isa, preset, 4 if isa == 'Amd64' else 1, '', 'SessionCompositor',
                    i8042=False, session_compositor=True, ui_font_absent=True, assignable=(),
                    suites=frozenset({'All', isa, isa + 'Session'}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')))

CASES += tuple(Case('%s.SessionInput' % isa, isa, preset, 4 if isa == 'Amd64' else 1, '', 'SessionInput',
                    i8042=True, session_input=True, assignable=(),
                    suites=frozenset({'All', isa, isa + 'Session'}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')))

# Native output and two maximize/restore clients.
CASES += (Case('Amd64.SessionLarge', 'Amd64', 'amd64', 4, '', 'SessionInput',
               session_input=True, session_large=True, assignable=(), edid=(2560, 1440),
               graphics_device='bochs-display', expected_display=(2560, 1440),
               suites=frozenset({'All', 'Amd64', 'Amd64Session'})),
          Case('Ia64.SessionLarge', 'Ia64', 'ia64', 1, '', 'SessionInput',
               session_input=True, session_large=True, assignable=(),
               expected_display=(640, 400),  # Firmware's mode.
               suites=frozenset({'All', 'Ia64', 'Ia64Session'})))

CASES += tuple(Case('%s.SessionStressSmp%d' % (isa, cpus), isa, preset, cpus, '', 'SessionStress',
                    i8042=True, session_input=True, session_stress=True, assignable=(), display_policy="1024x768",
                    suites=frozenset({'All', isa, isa + 'Session'}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')) for cpus in (2, 4))

CASES += tuple(Case('%s.SessionIdleSmp4' % isa, isa, preset, 4, '', 'SessionIdle',
                    session_input=True, assignable=(), display_policy='1024x768',
                    suites=frozenset({'All', isa, isa + 'Session'}))
               for isa, preset in (('Amd64', 'amd64'), ('Ia64', 'ia64')))

def describe(case: Case) -> str:
    """The --list row, including the disk recipe rather than just presence."""
    return '%-28s %-6s preset=%-34s firmware=%-13s cpus=%d disk=%-5s driver=%s%s' % (
        case.name, case.isa, case.preset, case.firmware, case.cpus, case.disk or 'no', case.driver,
        ' machine=' + case.machine if case.machine else '')


CASES += tuple(Case('%s.Power%s' % (isa, action.title()), isa, isa.lower(), 4, 'fat16', 'Power',
                    suites=frozenset({'Power', isa + 'Power'}), firmware=PRODUCTION_FIRMWARE,
                    power=action, assignable=(), i8042=isa == 'Amd64', usb_host='nec' if isa == 'Ia64' else '')
               for isa in ('Amd64', 'Ia64') for action in ('off', 'reboot'))


def select(suites: list[str], names: list[str]) -> list[Case]:
    """Cases in any of `suites` or named in `names` (glob patterns allowed)."""
    import fnmatch
    unknown = [suite for suite in suites if suite not in SUITES]
    if unknown:
        raise ValueError('unknown suites: %s (known: %s)' % (', '.join(unknown), ', '.join(SUITES)))
    selected = [case for case in CASES
                if any(suite in case.suites for suite in suites)
                or any(fnmatch.fnmatchcase(case.name, pattern) for pattern in names)]
    for pattern in names:
        if not any(fnmatch.fnmatchcase(case.name, pattern) for case in CASES):
            raise ValueError('no case matches %s' % pattern)
    return selected

# Std VGA uses multiples of eight pixels. Product USB uses bochs-display for 1366x768.
CASES += (Case('Amd64.DisplayNative', 'Amd64', 'amd64', 1, '', 'Amd64Default',
               suites=frozenset({'All', 'Amd64'}), edid=(1440, 900), expected_display=(1440, 900)),
          Case('Amd64.DisplayKeep', 'Amd64', 'amd64', 1, '', 'Amd64Default',
               suites=frozenset({'All', 'Amd64'}), edid=(1280, 800), display_policy='keep',
               expected_display=(1280, 800)))

CASES += (Case('Amd64.DisplayExplicit', 'Amd64', 'amd64', 1, '', 'Amd64Default',
               suites=frozenset({'All', 'Amd64'}), edid=(1440, 900), display_policy='1024x768',
               expected_display=(1024, 768)),)

# High RAM must be admitted and accounted from the map.
CASES += (Case('Amd64.Memory6G', 'Amd64', 'amd64', 1, '', 'Amd64Default',
               memory='6G', suites=frozenset({'All', 'Amd64'})),)

CASES += (Case('Amd64.MemoryRmrrBootServices', 'Amd64', 'amd64-preemption-probe', 1, '', 'MemoryRmrr',
               memory_rmrr=True, iommu='None', assignable=(), graphics_device='', firmware=PRODUCTION_FIRMWARE,
               suites=frozenset({'All', 'Amd64'})),)

# IF/PSR.i-clear CPU-0 lockups and a root-owned service recovery probe.
CASES += (Case('Amd64Watchdog.Lockup', 'Amd64', 'amd64-watchdog-probe', 4, '', 'Amd64Stop',
               suites=frozenset({'Amd64Watchdog'}), expected_stop='Lockup'),
          Case('Ia64Watchdog.Lockup', 'Ia64', 'ia64-watchdog-probe', 4, '', 'Ia64Stop',
               suites=frozenset({'Ia64Watchdog'}), expected_stop='Lockup'),
          Case('Amd64Watchdog.Component', 'Amd64', 'amd64-component-watchdog-probe', 4, '', 'ComponentWatchdog',
               suites=frozenset({'Amd64Watchdog'})))

# One FAT32 and one exFAT tree/view case on each ISA.
CASES += tuple(Case('%s.FsV2%s' % (isa, label), isa, preset, 1, recipe, driver,
                    i8042=isa == 'Amd64', suites=frozenset({'All', suite, 'FsV2'}))
               for isa, preset, driver, suite in (('Amd64', 'amd64', 'Amd64Default', 'Amd64'),
                                                   ('Ia64', 'ia64', 'Ia64Default', 'Ia64'))
               for label, recipe in (('Fat', 'fat32-v2'), ('Exfat', 'exfat-v2')))

# Product configuration, real broker seats, full native output.
CASES += tuple(Case(isa + '.Desktop' + seat, isa, isa.lower(), 1, '', 'ProductDesktop',
    firmware=PRODUCTION_FIRMWARE, product_desktop=True, session_face_withheld=seat == 'Fallback',
    i8042=isa == 'Amd64' or seat != 'Usb', xhci=seat == 'Usb' and isa == 'Amd64',
    product_media=seat == 'Usb' and isa == 'Amd64',
    usb_host='nec' if seat == 'Usb' and isa == 'Ia64' else '',
    edid=(2560, 1440) if isa == 'Amd64' else None,
    graphics_device='bochs-display' if isa == 'Amd64' else 'VGA',
    expected_display=(2560, 1440) if isa == 'Amd64' else (640, 400), assignable=(),
    suites=frozenset({'All', isa, isa + 'Session'}))
    for isa in ('Amd64', 'Ia64') for seat in ('Ps2', 'Usb', 'Fallback'))

CASES += tuple(Case(isa + '.DesktopPower' + action.title(), isa, isa.lower(), 4, 'fat16', 'ProductDesktop',
    firmware=PRODUCTION_FIRMWARE, product_desktop=True, power=action, assignable=(),
    i8042=isa == 'Amd64', usb_host='nec' if isa == 'Ia64' else '',
    expected_display=(640,400) if isa == 'Ia64' else None,
    suites=frozenset({isa + 'Session', isa + 'Power'}))
    for isa in ('Amd64','Ia64') for action in ('off','reboot'))

CASES += tuple(Case(isa + '.DesktopRecovery',isa,isa.lower(),1,'','ProductDesktop',
    firmware=PRODUCTION_FIRMWARE,product_desktop=True,session_compositor_fault=True,
    i8042=True,assignable=(),expected_display=(1280,800) if isa == 'Amd64' else (640,400),
    suites=frozenset({'All',isa,isa+'Session'})) for isa in ('Amd64','Ia64'))

# Watchdog.md: pause the entire hypervisor, then exercise
# the real desktop/input path on resumption with four admitted processors.
CASES += tuple(dataclasses.replace(
    next(case for case in CASES if case.name == isa + '.DesktopPs2'),
    name=isa + 'Watchdog.Pause' + phase.title(), cpus=4,
    driver='WholeMachineStall', whole_machine_pause=phase, tcg_thread='single',
    suites=frozenset({isa + 'Watchdog'}))
    for isa in ('Amd64', 'Ia64') for phase in ('early', 'session'))

# Retain MTTCG coverage too; all ordinary AMD64 SMP cases still use multi.
CASES += tuple(dataclasses.replace(case, name=case.name + 'Multi', tcg_thread='multi')
               for case in CASES if case.isa == 'Amd64' and case.whole_machine_pause)
# Each regression on both clients and seats.
CASES += tuple(dataclasses.replace(case,
    name=case.isa + '.Gui' + bug + ('DesktopUsb' if case.usb_console or case.usb_host else 'DesktopPs2' if case.product_desktop else 'SessionInput'),
    driver='GuiBugs', display_policy='1024x768' if case.session_input else case.display_policy,
    expected_display=(1024, 768) if case.session_input else case.expected_display,
    suites=frozenset({case.isa + 'Session'}))
    for case in CASES if case.name in ('Amd64.DesktopPs2', 'Amd64.DesktopUsb', 'Ia64.DesktopPs2', 'Ia64.DesktopUsb',
                                      'Amd64.SessionInput', 'Ia64.SessionInput')
    for bug in ('Maximize', 'Edge'))

# Dedicated, explicit synthetic-fixture test medium.
CASES += tuple(Case(isa+'.Login',isa,preset,1,'','Login',firmware='firmware-login',product_desktop=True,assignable=(),
    display_policy='800x600',expected_display=(800,600),suites=frozenset({'Login',isa+'Login'}))
    for isa,preset in (('Amd64','amd64'),('Ia64','ia64')))

CASES += tuple(Case(isa+'.LoginFailure.'+service,isa,preset,1,'','LoginFailure',firmware='firmware-login',product_desktop=True,assignable=(),login_failure=service,display_policy='800x600',expected_display=(800,600),suites=frozenset({'Login',isa+'Login'})) for isa,preset in (('Amd64','amd64'),('Ia64','ia64')) for service in ('auth','login','display'))

# Native-output toolkit client launched by the root-service preview.
CASES += tuple(Case(isa + '.ToolkitSession', isa, isa.lower(), 1, '', 'ToolkitSession',
                    suites=frozenset({'All', isa, 'ToolkitSession'}), toolkit_session=True,
                    product_desktop=True, assignable=(),
                    edid=(2560,1440) if isa == 'Amd64' else None,
                    graphics_device='bochs-display' if isa == 'Amd64' else 'VGA',
                    expected_display=(2560,1440) if isa == 'Amd64' else (640,400))
               for isa in ('Amd64', 'Ia64'))
