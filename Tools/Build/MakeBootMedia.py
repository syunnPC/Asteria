#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Make boot media for running a built ESP outside the VM runner.

The UEFI application (EFI/BOOT/BOOTX64.EFI or BOOTIA64.EFI) reads the
manifest, kernel and user images in EFI/ASTERIA before ExitBootServices. The whole
ESP directory tree, including opaque CRC-32C checked Vegur OTF data modules
and their FONTNOTE.TXT provenance, is copied to each medium:

- <name>.img: a raw GPT disk with one FAT32 EFI system partition, to write
  to a USB stick (`dd if=<name>.img of=/dev/sdX bs=4M conv=fsync`, or Rufus
  in DD mode) or to attach to a virtual machine as a disk;
- <name>-mbr.img: the same volume behind an MBR partition table (with
  --mbr). Written to a stick larger than the image, the GPT's backup header
  is not at the end of the stick, which some firmware takes as an invalid
  GPT; an MBR has no backup to misplace;
- <name>.vmdk: the same disk for VMware (with --vmdk);
- <name>.iso: an El Torito image whose EFI boot catalog entry is a FAT
  image holding the same files (with --iso). The ISO also exposes the ESP
  directory tree with ISO 9660 level-1 names for the IA-64 firmware reader.

Only user-space tools are used (sgdisk, mtools, qemu-img, xorriso); no
loop devices or root.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

SECTOR = 512
PARTITION_START = 2048            # 1 MiB, the usual alignment
ESP_TYPE = 'EF00'


def run(*command: str) -> None:
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def require(*tools: str) -> None:
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        sys.exit('missing tools: ' + ', '.join(missing))


def copy_tree(image: str, source: pathlib.Path) -> None:
    """Copy every file under source into the FAT image (mtools target)."""
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source).as_posix()
        if path.is_dir():
            run('mmd', '-i', image, '::/' + relative)
        else:
            run('mcopy', '-i', image, str(path), '::/' + relative)


def make_disk(esp: pathlib.Path, output: pathlib.Path, size_mib: int, mbr: bool = False) -> None:
    sectors = size_mib * 1024 * 1024 // SECTOR
    with open(output, 'wb') as disk:
        disk.truncate(sectors * SECTOR)
    if mbr:
        # One FAT32 (LBA) partition, as Rufus and Windows make UEFI sticks.
        last = sectors - 1
        subprocess.run(['sfdisk', '--quiet', '--label', 'dos', str(output)], check=True, stdout=subprocess.DEVNULL,
                       input='start=%d, size=%d, type=c, bootable\n' % (PARTITION_START, last - PARTITION_START + 1),
                       text=True)
    else:
        last = sectors - 34                 # The backup GPT's entries and header
        run('sgdisk', '--clear', '--new=1:%d:%d' % (PARTITION_START, last), '--typecode=1:' + ESP_TYPE,
            '--change-name=1:EFI system partition', str(output))
    offset = PARTITION_START * SECTOR
    partition = '%s@@%d' % (output, offset)
    # The hidden sectors are the partition's first LBA, as on any partitioned disk.
    run('mformat', '-i', partition, '-F', '-H', str(PARTITION_START), '-T', str(last - PARTITION_START + 1),
        '-v', 'ASTERIA', '::')
    copy_tree(partition, esp)


def make_iso(esp: pathlib.Path, output: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix='AsteriaIso-') as temporary:
        staging = pathlib.Path(temporary) / 'staging'
        staging.mkdir()
        shutil.copytree(esp, staging, dirs_exist_ok=True)
        boot = staging / 'EFIBOOT.IMG'
        payload = sum(path.stat().st_size for path in esp.rglob('*') if path.is_file())
        size_kib = max(2048, (payload * 2 // 1024 + 1023) // 1024 * 1024)
        with open(boot, 'wb') as image:
            image.truncate(size_kib * 1024)
        run('mformat', '-i', str(boot), '-v', 'ASTERIA', '::')
        copy_tree(str(boot), esp)
        run('xorriso', '-as', 'mkisofs', '-quiet', '-iso-level', '1', '-V', 'ASTERIA', '-e', 'EFIBOOT.IMG', '-no-emul-boot',
            '-o', str(output), str(staging))


def validate_esp(esp: pathlib.Path) -> None:
    """Refuse stale files before making a medium; absent UI modules remain valid."""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'Verify'))
    from AsteriaVerify import BootFiles
    directory = esp / 'EFI/ASTERIA'
    for logical, (name, checksum) in BootFiles.manifest((directory / 'BOOT.CFG').read_bytes()).items():
        if BootFiles.crc32c((directory / name).read_bytes()) != checksum:
            raise ValueError('%s: boot file CRC-32C mismatch' % logical)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--esp', type=pathlib.Path, required=True,
                        help='the ESP directory a preset builds, e.g. Build/CMake/amd64/FirmwareTest/Esp')
    parser.add_argument('--output', type=pathlib.Path, required=True, help='directory for the media')
    parser.add_argument('--name', default='Asteria')
    parser.add_argument('--size-mib', type=int, default=64, help='disk size; FAT32 needs at least 34 MiB')
    parser.add_argument('--vmdk', action='store_true')
    parser.add_argument('--iso', action='store_true')
    parser.add_argument('--mbr', action='store_true', help='also make <name>-mbr.img, partitioned with an MBR')
    args = parser.parse_args()
    if not any(args.esp.glob('EFI/BOOT/BOOT*.EFI')):
        sys.exit('%s holds no EFI/BOOT/BOOT*.EFI' % args.esp)
    try:
        validate_esp(args.esp)
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    if args.size_mib < 34:
        sys.exit('--size-mib must be at least 34 for FAT32')
    require('sgdisk', 'mformat', 'mcopy', 'mmd')
    args.output.mkdir(parents=True, exist_ok=True)
    disk = args.output / (args.name + '.img')
    make_disk(args.esp, disk, args.size_mib)
    print(disk)
    if args.mbr:
        require('sfdisk')
        legacy = args.output / (args.name + '-mbr.img')
        make_disk(args.esp, legacy, args.size_mib, mbr=True)
        print(legacy)
    if args.vmdk:
        require('qemu-img')
        vmdk = args.output / (args.name + '.vmdk')
        run('qemu-img', 'convert', '-f', 'raw', '-O', 'vmdk', str(disk), str(vmdk))
        print(vmdk)
    if args.iso:
        require('xorriso')
        iso = args.output / (args.name + '.iso')
        make_iso(args.esp, iso)
        print(iso)
    return 0


if __name__ == '__main__':
    sys.exit(main())
