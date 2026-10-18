# SPDX-License-Identifier: Apache-2.0
"""Bound the root service's scenario peaks using its linked native images.

shared executable segments: pages and Frame/MemoryObject pairs are retained once per image,
plus private segments and stacks per simultaneous incarnation. Scenario
order matters: networking precedes Directory/Client/interpreter staging.
physical memory management: the configured memory minimum is a lower bound on
allocatable RAM at boot; the runtime grant is the imported allocatable total.
Frame/MemoryObject pairs retain their fixed metadata quota.
"""
import argparse
import pathlib
import re
import struct


def constant(text: str, name: str, depth: int = 0) -> int:
    if depth >= 16:
        raise ValueError('recursive constant ' + name)
    match = re.search(r'\b%s\{([A-Za-z0-9_ *]+)\}' % re.escape(name), text)
    if not match:
        raise ValueError('missing numeric constant ' + name)
    value = 1
    for factor in match[1].split('*'):
        factor = factor.strip()
        value *= (int(factor.rstrip('Uu')) if re.fullmatch(r'[0-9]+[Uu]*', factor)
                  else constant(text, factor, depth + 1))
    return value


def layout(data: bytes) -> tuple[int, int, int, int]:
    """Shared/private pages, shared/private segment pairs (ELF64 LE)."""
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    offset, = struct.unpack_from('<Q', data, 32)
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride != 56:
        raise ValueError('invalid program header stride')
    totals = [0, 0, 0, 0]
    for index in range(count):
        kind, flags, _, address, _, _, size, _ = struct.unpack_from('<IIQQQQQQ', data, offset + index * stride)
        if kind == 1 and size:
            private = int(bool(flags & 2))
            totals[private] += (address % 4096 + size + 4095) // 4096
            totals[2 + private] += 1
    if not totals[2]:
        raise ValueError('no shared native segment')
    return tuple(totals)


def peaks(images, stack: int, window: int, foreign: int, dma: int, packets: int, isa: str, xhci=(68, 57, 256, 128),
          usb=(8, 4), display=None, probe=None, probe_pages=None, compositor=None, pattern=None, session_pages=0,
          input_seat=None, ahci_bounce=None):
    # The test and product scenarios stage the first four images; only the
    # product's USB host group driver stages the fifth, the usb image.
    usb_records = images[4:]
    images = images[:4]
    component, directory, client, interpreter = images
    separate_display = display is not None
    def shared(records):
        return sum(record[0] for record in records), sum(record[2] for record in records)
    def private(records):
        return sum(record[1] + stack for record in records), sum(record[3] + 1 for record in records)
    def scenario(retained, running, extra_pages=0, extra_pairs=0):
        if separate_display:
            retained = [*retained, display]
            running = [display, *running[1:]]
        sp, so = shared(retained)
        pp, po = private(running)
        return sp + pp + extra_pages, so + po + 1 + extra_pairs  # Scanout pair, no pages
    result = {
        'account': scenario([component], [component, component], window, 1),
        # Broker and edu task/service; two private DMA pages/pairs and one
        # registers pair. The retained component image is charged once.
        'edu DMA': scenario([component], [component, component], 2, 3),
        # After drain, the broker and shared image remain while the source
        # and target Frame pins wait for device-quiet confirmation.
        'edu parked pins': scenario([component], [component], 2, 3),
        'services': scenario([component, directory, client], [component, component, directory, client]),
        'interpreter': scenario(images, [component, interpreter], foreign, 1),
        'retained residue': shared([*images, display] if separate_display else images),
        # Last: all four images retained; display + broker + event client,
        # and the PS/2 driver (both ISAs publish the i8042).
        'input': scenario(images, [component] * 4),
        # Broker + block/filesystem drivers, command page, 16-page low bounce
        # Frame/MemoryObject and client-copy MemoryObject on both ISAs.
        'filesystem': scenario([component], [component] * 3, 1 + (ahci_bounce or 0),
                               5 if ahci_bounce else 3),
        # PowerOff.md: display + input broker + PS/2 + block + filesystem
        # remain alive together; the probe owns one temporary data Frame.
        'power PS/2 flush': scenario([component], [component] * 5, 2 + (ahci_bounce or 0),
                                     6 if ahci_bounce else 4),
        # Broker + NIC driver + network stack: the driver's descriptor and
        # packet pages (three runs, a MemoryObject for a client's Frame and
        # its registers' pair) and the stack's window pages on both ISAs.
        'network': scenario([component], [component] * 3, dma + packets, 5 + packets),
    }
    if usb_records:
        # Product input with the USB host group driver (UsbHostDriver.md):
        # display, broker and PS/2 from the component image, the driver from
        # the usb image, its DMA pages (one pair) and up to one register pair
        # per controller (device Frames charge no pages).
        usb_image = usb_records[0]
        dma_pages, controllers = usb
        result['USB host HID'] = scenario([component, usb_image], [component] * 3 + [usb_image],
                                          dma_pages, 1 + controllers)
        # IA-64 power VM profile uses USB alone, plus block and filesystem:
        # five components, USB DMA/registers and storage's existing extra pair.
        result['power USB flush'] = scenario([component, usb_image], [component] * 4 + [usb_image],
                                              dma_pages + 2 + (ahci_bounce or 0),
                                              1 + controllers + (6 if ahci_bounce else 4))
    if isa == 'Amd64':
        # PortsDemo runs in root with only the broker alive, after driver teardown
        # and before Directory/Client/interpreter staging. No DMA/window pages.
        result['ports'] = scenario([component], [component])
        base, state, maximum, shared_scratchpads = xhci
        result['xHCI HID'] = scenario([component], [component] * 4,
            base + state + maximum, 2 + (base + maximum + 31) // 32 + 1)
        # Two admitted controllers sharing this many scratchpad pages. Higher
        # simultaneous requests may exceed the grant and fail admission cleanly.
        dma_pairs = (2 * base + shared_scratchpads + 62) // 32
        result['two xHCI controllers (128 scratchpads shared)'] = scenario([component], [component] * 4,
            2 * (base + state) + shared_scratchpads, 4 + dma_pairs + 2)
        if usb_records:
            # Product init's five components : display, broker, PS/2 and one xHCI with as many
            # scratchpads as the two controllers above share, beside the USB host group with its DMA
            # pages and four register pairs. More scratchpads beside the group may fail the group's
            # admission cleanly within the grant.
            dma_pages, controllers = usb
            result['five components (128 scratchpads)'] = scenario(
                [component, usb_records[0]], [component] * 4 + [usb_records[0]],
                base + state + shared_scratchpads + dma_pages,
                2 + (base + shared_scratchpads + 31) // 32 + 1 + 1 + controllers)
    if probe is not None:
        # Root-started test probe: one 320x200, 63-page output in its own domain.
        # One RAM run, its probe MemoryObject and a broker read-only
        # MemoryObject; two extra pairs conservatively cover the three slots.
        if probe_pages is None:
            abi = (pathlib.Path(__file__).resolve().parents[2] / 'Source/Abi/ComponentBootstrap.hpp').read_text()
            probe_pages = constant(abi, 'ComponentSessionProbePages')
        result['session probe'] = scenario([component, probe], [component, probe], probe_pages, 2)
    if compositor is not None and pattern is not None:
        abi = (pathlib.Path(__file__).resolve().parents[2] / 'Source/Abi/ComponentBootstrap.hpp').read_text()
        sp, so = shared([display, compositor, pattern])
        pp, po = private([display, compositor, pattern, pattern])
        # Bootstrap MemoryObjects also map producer pools. Output adds one
        # pair, the broker's output MO and two read-only client pool MOs;
        # each client adds one Frame. The greater (MemoryObject) count wins.
        result['session compositor'] = sp + pp + session_pages + constant(abi, 'ComponentUiFontPages'), so + po + 5
    if input_seat is not None:
        sp, so = shared([display, compositor, pattern, input_seat])
        pp, po = private([display, compositor, pattern, input_seat, input_seat])
        abi = (pathlib.Path(__file__).resolve().parents[2] / 'Source/Abi/ComponentBootstrap.hpp').read_text()
        result['session input'] = sp + pp + constant(abi, 'ComponentCompositorPages') + constant(abi, 'ComponentPatternInputPages') + constant(abi, 'ComponentUiFontPages'), so + po + 5
    return result


def require(peaks, pages: int, pairs: int):
    for name, (used_pages, used_pairs) in peaks.items():
        if used_pages > pages or used_pairs > pairs:
            raise ValueError('%s needs %d pages/%d pairs, grant allows %d/%d' %
                             (name, used_pages, used_pairs, pages, pairs))


def require_ports(waitsets: int, interrupts: int):
    # The live display broker owns one WaitSet; PortsDemo creates one WaitSet
    # and one Interrupt. Its four boot-minted device authorities are not grant
    # creations. Earlier driver Interrupts have been destroyed and reaped.
    if waitsets < 2 or interrupts < 1:
        raise ValueError('ports needs 2 WaitSets/1 Interrupt, grant allows %d/%d' % (waitsets, interrupts))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=pathlib.Path, required=True)
    parser.add_argument('--isa', choices=('Amd64', 'Ia64'), required=True)
    # Component, directory, client and interpreter images, then the usb image where the build links
    # one.
    parser.add_argument('--display', type=pathlib.Path, required=True)
    parser.add_argument('--probe', type=pathlib.Path)
    parser.add_argument('--compositor', type=pathlib.Path)
    parser.add_argument('--pattern', type=pathlib.Path)
    parser.add_argument('--input-seat', type=pathlib.Path)
    parser.add_argument('--toolkit', type=pathlib.Path)
    parser.add_argument('images', type=pathlib.Path, nargs='+', metavar='ELF')
    args = parser.parse_args()
    if len(args.images) not in (4, 5):
        raise ValueError('expected four or five images')
    abi = (args.root / 'Source/Abi/ComponentBootstrap.hpp').read_text()
    amd64 = args.isa == 'Amd64'
    source = ('Source/Platform/Amd64/Boot/Amd64BootRuntime.cpp' if amd64 else
              'Source/Platform/Ia64/System/Ia64PagerRuntime.cpp')
    grant = (args.root / source).read_text()
    prefix = 'PagerGrant' if amd64 else 'Grant'
    pages = constant(grant, 'PagerMinimumPages' if amd64 else 'MinimumGrantPages')
    pairs = constant(grant, prefix + 'MemoryObjects')
    stack = constant(abi, ('Component' if amd64 else 'Ia64Component') + 'StackPages') + 1
    window = constant(abi, ('Component' if amd64 else 'Ia64Component') + 'WindowPages')
    samples = [args.images[0].parent / 'Samples' / (name + '-sample.bin')
               for name in (('ia64' if amd64 else 'amd64'), 'aarch64', 'pe')]
    if not all(path.is_file() for path in samples):
        raise ValueError('missing module foreign samples')
    # Module storage belongs to the kernel's retained loader receipt, outside
    # the pager grant. Bound the complete table independently of the PT_LOAD
    # resource peaks. Linked file sizes conservatively include symbol tables.
    module_abi = (args.root / 'Source/Abi/ComponentImageTable.hpp').read_text()
    pager_abi = (args.root / 'Source/Abi/PagerBootstrap.hpp').read_text()
    all_images = [*args.images, args.display] + ([args.probe] if args.probe else []) + ([args.compositor, args.pattern] if args.compositor and args.pattern else [])
    if args.input_seat: all_images.append(args.input_seat)
    if args.toolkit: all_images.append(args.toolkit)
    # Desktop session personalization: opaque font receipts consume table entries and retained boot
    # bytes too, independently of the root's page grant.
    fonts = [args.root / 'Data/Fonts/Vegur' / name for name in ('Vegur-Bold.otf', 'Vegur-Regular.otf')]
    if any(not path.is_file() or path.stat().st_size > constant(abi, 'ComponentUiFontFaceBytes') for path in fonts):
        raise ValueError('UI font receipt exceeds its bounded slot or is missing')
    modules = [*all_images, *samples, *fonts]
    count = len(modules)
    if count > constant(module_abi, 'ComponentImageMaximumCount'):
        raise ValueError('module table capacity exceeded')
    module_bytes = 16 + count * 32 + sum((path.stat().st_size + 15) & ~15 for path in modules)
    module_bytes = (module_bytes + 4095) & ~4095
    if module_bytes > constant(pager_abi, 'PagerModuleMaximumBytes'):
        raise ValueError('module table exceeds kernel mapping range')
    print('%s module table: at most %d bytes, %d entries; outside pager grant' % (args.isa, module_bytes, count))
    foreign = max((sample.stat().st_size + 4095) // 4096 for sample in samples)
    dma = 1 + constant(abi, 'NicReceiveBufferPages') + constant(abi, 'NicTransmitBufferPages')
    packets = constant(abi, 'NetworkWindowPages')
    scenarios = peaks([layout(image.read_bytes()) for image in args.images], stack, window, foreign, dma, packets,
                      args.isa, (68, constant(abi, 'XhciStatePages'), constant(abi, 'XhciScratchpadBudget'), 128),
                      (2 * constant(abi, 'UsbHostControllers'), constant(abi, 'UsbHostControllers')),
                      display=layout(args.display.read_bytes()),
                      probe=layout(args.probe.read_bytes()) if args.probe else None,
                      probe_pages=constant(abi, 'ComponentSessionProbePages'),
                      compositor=layout(args.compositor.read_bytes()) if args.compositor else None,
                      pattern=layout(args.pattern.read_bytes()) if args.pattern else None,
                      input_seat=layout(args.input_seat.read_bytes()) if args.input_seat else None,
                      session_pages=sum(constant(abi, name) for name in ('ComponentCompositorPages', 'ComponentPatternPages', 'ComponentPatternSecondaryPages')),
                      ahci_bounce=constant(abi, 'AhciBouncePages'))
    for name, (used_pages, used_pairs) in scenarios.items():
        print('%s %s: %d/%d pages, %d/%d pairs' % (args.isa, name, used_pages, pages, used_pairs, pairs))
    require(scenarios, pages, pairs)
    if amd64:
        waitsets = constant(grant, 'PagerGrantWaitSets')
        interrupts = constant(grant, 'PagerGrantInterrupts')
        require_ports(waitsets, interrupts)
        print('Amd64 ports: 2/%d WaitSets, 1/%d Interrupts' % (waitsets, interrupts))
    print('%s minimum grant with four-page/one-pair growth margin: %d pages, %d pairs' %
          (args.isa, max(value[0] for value in scenarios.values()) + 4,
           max(value[1] for value in scenarios.values()) + 1))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, struct.error) as error:
        raise SystemExit(str(error)) from error
