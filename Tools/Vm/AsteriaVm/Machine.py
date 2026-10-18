# SPDX-License-Identifier: Apache-2.0
"""QEMU machine profiles and per-case run directories.

Each case runs in its own directory with its own copy of the EFI system
partition, firmware variable store and disk fixture, so cases never share a
writable file and can run concurrently. QMP and the guest debug channel use
socket pairs inherited by QEMU, so there are no socket files, no path-length
limits and no startup polling.
"""
from __future__ import annotations

import dataclasses
import os
import pathlib
import shutil
import socket
import sys
import subprocess

# The AHCI READ DMA EXT fixture: a 2 MiB raw disk whose sector 8 holds this
# token 16 times (512 bytes). Both kernels read LBA 8 during boot.
DISK_SIZE_BYTES = 2 * 1024 * 1024
DISK_TOKEN = b'ASTERIA-READ-DMA-EXT-SECTOR-0001'
DISK_TOKEN_SECTOR = 8
# The disk's block backend, which QMP names to throttle it.
DISK_DRIVE = 'ahci0'
DISK_SERIAL = 'ASTERIAAHCI0001'
DISK_MODEL = 'ASTERIA AHCI DISK'
# The fixture is the only disk, on the first port of the machine's AHCI
# controller (`ide.0`: Q35's ICH9 AHCI, the itanium2-vpc AHCI).
DISK_AHCI_PORT = 0
# The RNG device of each machine profile that has one. On AMD64, OVMF's
# VirtioRngDxe publishes EFI_RNG_PROTOCOL for it, which the boot application
# credits as its firmware entropy source (BootEntropy). The IA-64 firmware has
# no RNG protocol; that profile relies on the cycle-counter fallback.
RNG_DEVICE = {'Amd64': 'virtio-rng-pci'}
# The e1000e NIC the test firmware assigns to its network driver
# (Source/User/Network/NetworkStack.hpp, and its IA-64
# ): no option ROM, a fixed slot clear of the automatically placed
# devices (on IA-64 the slot whose INTA is GSI 16, which no other function
# uses), and a fixed MAC.
NIC_ADDRESS = '0x10'
NIC_MAC = '52:54:00:a5:7e:01'
# The VT-d test machine of the IA-64 fork's branch build: a 7500-class hub
# whose two units sit at VTBAR and VTBAR + 0x1000, slot 7 holding its
# secondary bridge (so no virtio-iommu there), and a Tukwila CPU that
# reports 50 physical-address bits. The CPU model is pinned like montecito.
IA64_VTD_MACHINE = 'itanium2-vpc-iommu'
IA64_VTD_CPU = 'tukwila-9350'
# The USB 1.1/2.0 host controllers of the USB host group driver
# by controller set: an ICH9
# EHCI with UHCI companions (AMD64 q35 at 1d.x like the real ICH9/ICH10 of
# the rx2800 i2; IA-64 slot 9 with one companion) or, on IA-64, a NEC EHCI
# with one OHCI companion in slot 8 (the zx-family boxes' NEC parts). Each
# companion owns two EHCI ports in order. QEMU's usb-kbd and usb-mouse are
# high speed by default (usb_version=2) and stay on the EHCI; usb_version=1
# makes one full speed, which the EHCI releases to the port's companion.
USB_HOST_CONTROLLERS = {
    ('Amd64', 'ich9'): ['ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on',
                        'ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on',
                        'ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1'],
    ('Ia64', 'ich9'): ['ich9-usb-ehci1,id=ehci,addr=9.7,multifunction=on',
                       'ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=9.0,multifunction=on'],
    ('Ia64', 'nec'): ['nec-usb-ehci,id=ehci,addr=8.0,multifunction=on,num-ports=6',
                      'nec-usb-ohci,masterbus=ehci.0,firstport=0,num-ports=4,addr=8.1'],
}
# Where the keyboard and the mouse go, by layout: (EHCI port, full speed).
# `split` puts a high-speed keyboard on the EHCI and a full-speed mouse on a
# companion, `companion` both full speed on one companion, `ehci` both high
# speed on the EHCI, and `swap` a full-speed keyboard on a companion and a
# high-speed mouse on the EHCI; `none` attaches neither (the controllers
# alone, beside an xHCI that carries the keyboard and the mouse).
USB_HOST_LAYOUTS = {
    'split': ((5, False), (3, True)),
    'companion': ((1, True), (2, True)),
    'ehci': ((5, False), (6, False)),
    'swap': ((1, True), (3, False)),
    'none': (),
}


def usb_host_devices(isa: str, controllers: str, layout: str, console: bool) -> list[str]:
    """The -device arguments of a USB host group case: the controllers, then
    usb-kbd and usb-mouse on their EHCI ports; console binds the keyboard to
    the display QMP names (`usbconsole`)."""
    arguments = []
    for device in USB_HOST_CONTROLLERS[(isa, controllers)]:
        arguments += ['-device', device]
    if not USB_HOST_LAYOUTS[layout]:
        return arguments
    (keyboard, keyboard_full), (mouse, mouse_full) = USB_HOST_LAYOUTS[layout]
    arguments += ['-device', 'usb-kbd,id=usbkbd,bus=ehci.0,port=%d%s%s' % (
                      keyboard, ',usb_version=1' if keyboard_full else '', ',display=usbconsole' if console else ''),
                  '-device', 'usb-mouse,id=usbmouse,bus=ehci.0,port=%d%s' % (
                      mouse, ',usb_version=1' if mouse_full else '')]
    return arguments


@dataclasses.dataclass(frozen=True)
class Environment:
    """Host programs and firmware; each has an environment-variable override."""
    ia64_qemu: str
    ia64_firmware: str
    ia64_bios_directory: str
    amd64_qemu: str
    amd64_firmware_code: str
    amd64_firmware_variables: str
    # The fork's VT-d branch build, used only by IA-64 cases that name a
    # machine (Case.machine); the default IA-64 machine keeps the ones above.
    ia64_vtd_qemu: str = 'qemu-system-ia64-vtd'
    ia64_vtd_firmware: str = ''
    ia64_vtd_bios_directory: str = ''

    @classmethod
    def from_environment(cls) -> 'Environment':
        get = os.environ.get
        # Match the code and variable-store templates from one installation.
        code, variables = next(((str(directory / code), str(directory / variables))
                                for directory, code, variables in (
                                    (pathlib.Path('/usr/share/OVMF'), 'OVMF_CODE_4M.fd', 'OVMF_VARS_4M.fd'),
                                    (pathlib.Path('/usr/share/edk2/ovmf'), 'OVMF_CODE_4M.fd', 'OVMF_VARS_4M.fd'),
                                    (pathlib.Path('/usr/share/edk2/x64'), 'OVMF_CODE.4m.fd', 'OVMF_VARS.4m.fd'))
                                if (directory / code).is_file() and (directory / variables).is_file()), ('', ''))
        return cls(
            get('IA64_QEMU', 'qemu-system-ia64'),
            get('IA64_FIRMWARE', ''),
            get('IA64_BIOS_DIRECTORY', ''),
            get('AMD64_QEMU', 'qemu-system-x86_64'),
            get('AMD64_FIRMWARE_CODE', code),
            get('AMD64_FIRMWARE_VARIABLES', variables),
            get('IA64_VTD_QEMU', cls.ia64_vtd_qemu),
            get('IA64_VTD_FIRMWARE', cls.ia64_vtd_firmware),
            get('IA64_VTD_BIOS_DIRECTORY', cls.ia64_vtd_bios_directory))

    def validate(self, isa: str, machine: str = '') -> None:
        """Check only the programs and firmware needed by a selected case."""
        if isa == 'Ia64':
            prefix = 'IA64_VTD' if machine and machine != 'hp-i2000' else 'IA64'
            fields = ((prefix + '_QEMU', getattr(self, prefix.lower() + '_qemu'), 'program'),
                      (prefix + '_FIRMWARE', getattr(self, prefix.lower() + '_firmware'), 'file'),
                      (prefix + '_BIOS_DIRECTORY', getattr(self, prefix.lower() + '_bios_directory'), 'directory'))
        elif isa == 'Amd64':
            fields = (('AMD64_QEMU', self.amd64_qemu, 'program'),
                      ('AMD64_FIRMWARE_CODE', self.amd64_firmware_code, 'file'),
                      ('AMD64_FIRMWARE_VARIABLES', self.amd64_firmware_variables, 'file'))
        else:
            raise ValueError('unknown ISA: ' + isa)
        for name, value, kind in fields:
            if kind == 'directory' and not value:
                continue  # QEMU uses its installed BIOS search directory.
            available = (shutil.which(value) if kind == 'program'
                         else pathlib.Path(value).is_dir() if kind == 'directory'
                         else bool(value) and pathlib.Path(value).is_file())
            if not available:
                hint = ' or add the program to PATH' if kind == 'program' else ''
                raise ValueError(f'{name}: {kind} unavailable ({value or "not configured"}); set {name}{hint}')


@dataclasses.dataclass
class Launch:
    argv: list[str]
    qmp: socket.socket          # Our end of the QMP socket pair
    debug: socket.socket        # Our end of the debug-channel socket pair
    inherited: list[socket.socket]  # QEMU's ends, closed after spawning
    lan: socket.socket | None = None  # Our end of the NIC's datagram link: one Ethernet frame per datagram


def write_disk_fixture(path: pathlib.Path) -> bytes:
    """Creates the AHCI fixture and returns the expected sector contents."""
    sector = DISK_TOKEN * (512 // len(DISK_TOKEN))
    with path.open('wb') as handle:
        handle.truncate(DISK_SIZE_BYTES)
        handle.seek(DISK_TOKEN_SECTOR * 512)
        handle.write(sector)
    return sector


def _socket_pair() -> tuple[socket.socket, socket.socket]:
    ours, theirs = socket.socketpair()
    theirs.set_inheritable(True)
    return ours, theirs


def prepare(isa: str, esp: pathlib.Path, run_directory: pathlib.Path, cpus: int, disk: str,
            environment: Environment, device: bool = False, i8042: bool = True,
            x2apic: bool = False, pit: bool = True, iommu: str = 'Default', aml: str = '', xhci: bool = False,
            usb_boot: bool = False, xhci_failure: bool = False, edid: tuple[int, int] | None = None,
            display_policy: str = '', graphics_device: str = 'VGA', fadt8042: bool = True,
            floating_keyboard: bool = False, product_media: bool = False, xhci_large: bool = False,
            machine: str = '', machine_options: str = '', trust: tuple[str,...] = (),
            assignable: tuple[str,...] = ('edu', 'e1000e'), usb_host: str = '', usb_layout: str = 'split',
            synthetic_rmrr: bool = False, session_probe: bool = False, session_compositor: bool = False,
            session_input: bool = False, session_large: bool = False, session_stress: bool = False, memory: str = '', memory_rmrr: bool = False,
            fault_channel: bool = False, ui_font_absent: bool = False, reboot: bool = False,
            product_desktop: bool = False, tcg_thread: str = 'multi', toolkit_session: bool = False) -> Launch:
    """Populates the run directory and returns the QEMU command line. With
    `device`, both ISAs attach QEMU's edu, the device the test firmware assigns
    to a driver, and an e1000e
    whose network is a datagram socket pair the runner holds the other end
    of; `assignable` names which of the
    two each attaches. An IA-64 `machine` other than `hp-i2000` runs on the
    VT-d branch build with `machine_options` appended to its properties;
    `hp-i2000` runs on the default build. `trust` appends a BOOT.CFG trust
    entry for each driver's function (AsteriaVm.Trust). `usb_host` attaches
    a USB host controller set (USB_HOST_CONTROLLERS) with usb-kbd and
    usb-mouse placed by `usb_layout`. `synthetic_rmrr` (AMD64) attaches the
    two Q35 ICH9 EHCIs at 1a.0 and 1d.0 and an authored DMAR
    whose RMRR reserves both (AsteriaVm.Dmar), with no IOMMU device."""
    if machine and isa != 'Ia64':
        raise ValueError('unknown %s machine %s' % (isa, machine))
    if synthetic_rmrr and (isa != 'Amd64' or device or usb_host or xhci):
        raise ValueError('the synthetic-RMRR machine is AMD64 only and has no IOMMU device for other DMA devices')
    if machine == 'hp-i2000' and (disk or device or iommu not in ('Default', 'None') or aml or machine_options):
        raise ValueError('hp-i2000 cases take no disk, device, IOMMU, AML or machine options')
    run_directory.mkdir(parents=True, exist_ok=True)
    case_esp = run_directory / 'Esp'
    if case_esp.exists():
        shutil.rmtree(case_esp)
    if tcg_thread not in ('single', 'multi'):
        raise ValueError('invalid TCG thread mode: ' + tcg_thread)
    shutil.copytree(esp, case_esp)
    # Console cases explicitly select console mode.
    # Test firmware manifests omit session; product media itself defaults desktop.
    if not product_desktop:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        if manifest.is_file():
            manifest.write_text(manifest.read_text().replace('session = desktop', 'session = console'))
    if fault_channel:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        text = manifest.read_text()
        if 'module = face-probe ' not in text:
            raise ValueError('test media lacks the probe activation receipt')
        manifest.write_text(text.replace('module = face-probe ', 'module = fault-channel '))
    if session_compositor or session_input:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        text = manifest.read_text()
        if 'module = compositor ' not in text:
            raise ValueError('test media lacks the compositor')
        manifest.write_text(text.replace('module = compositor ', 'module = session-stress ' if session_stress else 'module = session-large ' if session_large else 'module = session-input ' if session_input else 'module = session-test '))
    if toolkit_session:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        text = manifest.read_text()
        if 'module = toolkit ' not in text:
            raise ValueError('test media lacks the toolkit preview')
        text = text.replace('module = toolkit ', 'module = toolkit-session ')
        if 'session = desktop' not in text:
            text += '\nsession = desktop\n'
        manifest.write_text(text)
    if ui_font_absent:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        manifest.write_text(''.join(line for line in manifest.read_text().splitlines(keepends=True)
                                    if not line.startswith(('module = ui-bold ', 'module = ui-regular '))))
    if session_probe:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        text = manifest.read_text()
        if 'module = face-probe ' not in text:
            raise ValueError('test media lacks the session probe')
        manifest.write_text(text.replace('module = face-probe ', 'module = session-probe '))
    if display_policy:
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        manifest.write_text(manifest.read_text() + '\ndisplay = %s\n' % display_policy)
    if trust:
        from AsteriaVm import Trust
        manifest = case_esp / 'EFI/ASTERIA/BOOT.CFG'
        manifest.write_text(manifest.read_text() + ''.join('%s\n' % line
                                                           for line in Trust.lines(isa, trust, esp, case_esp)))
    if aml:
        (case_esp / 'EFI/ASTERIA/ACPI').mkdir(parents=True, exist_ok=True)
        # Fresh case media cannot satisfy the oracle with an earlier report.
        for path in case_esp.rglob('*'):
            if path.is_file() and path.relative_to(case_esp).as_posix().casefold() in (
                    'efi/asteria/acpi.txt', 'efi/asteria/noaml'):
                path.unlink()
        if aml == 'skip':
            marker = case_esp / 'EFI/ASTERIA/NOAML'
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.touch()
    marker = case_esp / 'EFI/ASTERIA/NOFADT8042'
    if not fadt8042:
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()
    elif marker.exists():
        marker.unlink()
    if disk:
        from AsteriaVm.DiskRecipes import write_disk_recipe
        fixture = run_directory / 'Disk.raw'
        write_disk_recipe(fixture, disk)
    qmp, qmp_peer = _socket_pair()
    debug, debug_peer = _socket_pair()
    inherited = [qmp_peer, debug_peer]
    lan = None

    def nic() -> list[str]:
        # The NIC's network: one Ethernet frame per datagram on a socket
        # pair whose other end the runner keeps (AsteriaVm.Lan).
        nonlocal lan
        lan, lan_peer = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        lan_peer.set_inheritable(True)
        inherited.append(lan_peer)
        return ['-netdev', 'dgram,id=lan,local.type=fd,local.str=%d' % lan_peer.fileno(),
                '-device', 'e1000e,netdev=lan,rombar=0,addr=%s,mac=%s' % (NIC_ADDRESS, NIC_MAC)]
    common = [
        '-smp', str(cpus), '-m', memory or ('2048M' if usb_boot else '512M'),
        '-chardev', 'socket,id=qmp,fd=%d' % qmp_peer.fileno(), '-mon', 'chardev=qmp,mode=control',
        '-chardev', 'socket,id=dbg,fd=%d' % debug_peer.fileno(),
        '-serial', 'file:%s' % (run_directory / 'Serial.log'),
        '-display', 'none', '-nic', 'none',
        '-action', 'reboot=reset' if reboot else 'reboot=shutdown',
        '-S',
    ]
    if isa == 'Ia64' and machine == 'hp-i2000':
        # QEMU's HP i2000 (460GX, Merced). It has no ACPI debug port: the
        # firmware's SPCR and HCDP name COM1, its console, which the kernel
        # then uses as its diagnostic channel (BootProtocol.md, "Debug
        # port"), so the serial port is the channel. The fork's -debug-port
        # creates its chardev there but attaches no UART and publishes no
        # DBGP; DebugPort.log keeps whatever it would carry, which the driver
        # requires to be nothing (a machine that grows a DBGP port moves the
        # kernel's lines there, and the case says so). Its graphics, i8042,
        # IDE and SCSI controllers are built in; it has no ECAM, AHCI or
        # IOMMU. Its USB keyboard and tablet are left out (usb=off) so QMP
        # input reaches the i8042. Production firmware only: the test
        # firmware's devices are PCI functions the boot binds through ECAM.
        common[common.index('-serial') + 1] = 'chardev:dbg'
        common[common.index('-m') + 1] = '2048M'
        argv = [environment.ia64_qemu, '-machine', 'hp-i2000,nvram=none,usb=off', '-cpu', 'merced',
                '-accel', 'tcg,thread=single', '-bios', environment.ia64_firmware,
                *(['-L', environment.ia64_bios_directory] if environment.ia64_bios_directory else []),
                '-drive', 'file=fat:ro:%s,format=raw,readonly=on' % case_esp,
                '-chardev', 'file,id=dbgport,path=%s' % (run_directory / 'DebugPort.log'),
                '-debug-port', 'chardev:dbgport', *common]
    elif isa == 'Ia64':
        if aml:
            media = run_directory / 'BootMedia'
            subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve().parents[2] / 'Build/MakeBootMedia.py'),
                            '--esp', str(case_esp), '--output', str(media), '--mbr'],
                           check=True, capture_output=True)
            storage = ['-drive', 'file=%s,format=raw' % (media / 'Asteria-mbr.img')]
        else:
            storage = ['-drive', 'file=fat:ro:%s,format=raw,readonly=on' % case_esp]
        if disk:
            iso = run_directory / 'Esp.iso'
            subprocess.run(['xorriso', '-as', 'mkisofs', '-quiet', '-iso-level', '1', '-V', 'ASTERIA_ESP',
                            '-o', str(iso), str(case_esp)], check=True, capture_output=True)
            storage = [
                '-drive', 'if=none,id=esp-cd,file=%s,format=raw,readonly=on' % iso,
                '-device', 'scsi-cd,drive=esp-cd,bus=scsi.0',
                '-drive', 'if=none,id=%s,file=%s,format=raw' % (DISK_DRIVE, fixture),
                '-device', 'ide-hd,drive=%s,bus=ide.%d,serial=%s,model=%s' % (DISK_DRIVE, DISK_AHCI_PORT, DISK_SERIAL,
                                                                                 DISK_MODEL),
            ]
        # Built-in controllers occupy slots 1..5. edu's INTA is GSI 22;
        # the polled IOMMU's is GSI 23; the e1000e's, at slot 0x10, GSI 16.
        # On the VT-d machine slot 7 is the hub's secondary bridge instead,
        # and edu sits behind the INCLUDE_PCI_ALL unit.
        branch = bool(machine)
        properties = ','.join(part for part in (machine or 'itanium2-vpc',
                                                'firmware-console=serial,nvram=none,pci-intx-lines=8,i8042='
                                                + ('on' if i8042 else 'off'),
                                                machine_options) if part)
        virtio = iommu == 'Default' and machine != IA64_VTD_MACHINE
        bios_directory = environment.ia64_vtd_bios_directory if branch else environment.ia64_bios_directory
        argv = [environment.ia64_vtd_qemu if branch else environment.ia64_qemu,
                '-machine', properties,
                '-cpu', IA64_VTD_CPU if machine == IA64_VTD_MACHINE else 'montecito', '-accel', 'tcg,thread=single',
                '-bios', environment.ia64_vtd_firmware if branch else environment.ia64_firmware,
                *(['-L', bios_directory] if bios_directory else []),
                *storage, *(['-device', 'edu,addr=0x6'] if device and 'edu' in assignable else []),
                *(nic() if device and 'e1000e' in assignable else []),
                *(['-device', 'virtio-iommu-pci,granule=4k,addr=0x7'] if virtio else []),
                *(usb_host_devices(isa, usb_host, usb_layout, False) if usb_host else []),
                '-debug-port', 'chardev:dbg', '-vga', 'ati',
                *common]
    elif isa == 'Amd64':
        variables = run_directory / 'Variables.fd'
        shutil.copyfile(environment.amd64_firmware_variables, variables)
        variables.chmod(0o644)
        storage = ['-drive', 'if=virtio,file=fat:rw:%s,format=raw' % case_esp]
        if product_media:
            media = run_directory / 'ProductMedia'
            subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve().parents[2] / 'Build/MakeBootMedia.py'),
                            '--esp', str(case_esp), '--output', str(media), '--name', 'Asteria-product', '--mbr'],
                           check=True, capture_output=True)
            storage = ['-drive', 'if=virtio,file=%s,format=raw' % (media / 'Asteria-product-mbr.img')]
        if usb_boot:
            if not xhci:
                raise ValueError('USB boot requires xHCI')
            media = run_directory / 'BootMedia'
            subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve().parents[2] / 'Build/MakeBootMedia.py'),
                            '--esp', str(case_esp), '--output', str(media), '--mbr'],
                           check=True, capture_output=True)
            storage = ['-drive', 'if=none,id=usbboot,file=%s,format=raw' % (media / 'Asteria-mbr.img')]
        if disk:
            storage += ['-drive', 'if=none,id=%s,format=raw,file=%s' % (DISK_DRIVE, fixture),
                        '-device', 'ide-hd,drive=%s,bus=ide.%d,serial=%s,model=%s' % (
                            DISK_DRIVE, DISK_AHCI_PORT, DISK_SERIAL, DISK_MODEL)]
        # Qemu64 is pinned: the kernel's FXSAVE/SSE policy needs FXSR, SSE and SSE2, and an unpinned
        # default model may change with QEMU. The assignable device behind a VT-d unit; interrupt
        # remapping stays off.
        devices = []
        if xhci_failure:
            iommu = 'None'
        iommu_devices = (['-device', 'AMDVI-PCI,id=amdvi,addr=06.0',
                          '-device', 'amd-iommu,pci-id=amdvi,dma-remap=on,intremap=off'] if iommu == 'AmdVi'
                         else [] if iommu == 'None' else ['-device', 'intel-iommu,intremap=off'])
        if device:
            devices = [*iommu_devices, *(['-device', 'edu'] if 'edu' in assignable else [])]
            if 'e1000e' in assignable:
                devices += nic()
        if usb_host:
            if not device:
                devices += iommu_devices
            devices += usb_host_devices(isa, usb_host, usb_layout, True)
        if memory_rmrr:
            from AsteriaVm import Dmar
            (run_directory / 'Dmar.aml').write_bytes(Dmar.boot_services_table())
            devices += ['-acpitable', 'file=%s' % (run_directory / 'Dmar.aml')]
        if synthetic_rmrr:
            from AsteriaVm import Dmar
            (run_directory / 'Dmar.aml').write_bytes(Dmar.table())
            devices += ['-device', 'ich9-usb-ehci2,id=ehci2,addr=1a.0',
                        '-device', 'ich9-usb-ehci1,id=ehci1,addr=1d.0',
                        '-acpitable', 'file=%s' % (run_directory / 'Dmar.aml')]
        if xhci:
            # One IOMMU confines every driver (an xHCI beside a USB host set).
            if not device and not usb_host:
                devices += iommu_devices
            controller = ('qemu-xhci,id=xhci,addr=08.0' if usb_boot
                          else 'qemu-xhci,id=xhci,addr=08.0,msi=on,msix=off')
            if xhci_large:
                controller += ',p2=15,p3=15'
            elif xhci_failure:
                controller += ',p2=9,p3=9'
            devices += ['-device', controller,
                        *(['-device', 'usb-storage,bus=xhci.0,drive=usbboot'] if usb_boot else []),
                        '-device', 'usb-kbd,id=usbkbd,bus=xhci.0,display=usbconsole' + (',port=14' if xhci_large else ''),
                        '-device', 'usb-mouse,id=usbmouse,bus=xhci.0' + (',port=15' if xhci_large else '')]
        argv = [environment.amd64_qemu,
                '-accel', 'tcg,thread=' + tcg_thread,
                '-machine', 'q35' + (',kernel-irqchip=split' if usb_boot else '')
                + ('' if i8042 else ',i8042=off') + ('' if pit else ',pit=off,hpet=on'),
                '-cpu', 'max,+x2apic' if x2apic else 'max' if usb_boot else 'qemu64', '-nodefaults',
                *(['-device', graphics_device + (',id=usbconsole' if (xhci or usb_host) else '')
                   + ',edid=on,xres=%d,yres=%d' % (edid or (1280, 800))] if graphics_device else []),
                '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=%s' % environment.amd64_firmware_code,
                '-drive', 'if=pflash,format=raw,unit=1,file=%s' % variables,
                *storage, *devices,
                *(['-acpitable', 'file=%s' % (pathlib.Path(__file__).resolve().parents[3]
                    / 'Tests/Acpi/Aml/Fixtures/FloatingKeyboard.aml')] if floating_keyboard else []),
                '-device', 'isa-debugcon,iobase=0x402,chardev=dbg',
                '-device', RNG_DEVICE['Amd64'],
                '-audiodev', 'none,id=noaudio', *common]
    else:
        raise ValueError('unknown ISA %s' % isa)
    return Launch(argv, qmp, debug, inherited, lan)


def bound_to_runner(argv: list[str]) -> list[str]:
    """Makes the kernel kill the program when the thread that starts it ends.

    A runner that is killed (a `timeout` wrapper, SIGKILL, a crash) would
    otherwise leave its QEMU processes running without a client. util-linux
    `setpriv` sets the parent-death signal and executes the program, so the
    signal stays tied to the runner's worker thread, which waits for QEMU
    before it takes another case.
    """
    return ['setpriv', '--pdeathsig', 'KILL', '--', *argv] if shutil.which('setpriv') else argv


def spawn(launch: Launch, run_directory: pathlib.Path) -> subprocess.Popen:
    log = (run_directory / 'Qemu.log').open('wb')
    try:
        process = subprocess.Popen(bound_to_runner(launch.argv), stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, pass_fds=[peer.fileno() for peer in launch.inherited])
    finally:
        log.close()
        for peer in launch.inherited:
            peer.close()
    (run_directory / 'Command.txt').write_text(' '.join(launch.argv) + '\n')
    return process
