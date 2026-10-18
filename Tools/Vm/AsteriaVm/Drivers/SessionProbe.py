# SPDX-License-Identifier: Apache-2.0
"""Root-started, sub-screen broker session proof."""
from __future__ import annotations

import json
import struct
import time

from AsteriaVm import Build  # Configures the project verifier import path
from AsteriaVerify import Layout
from AsteriaVm import Amd64Layout, Display, Evidence, Guest, Ia64Layout, Session
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import Amd64Evidence, Amd64Paging, Ia64Evidence, Ia64State, SharedImages, UserProcessors

PREFIX = 'USER:# session-probe: '


def _record(context, name):
    path = context.artifact(name + '.bin')
    error = Evidence.save(context.qmp, context.console.lines, path)
    context.require('session.evidence.' + name, error is None, error or '')
    schema = (Amd64Evidence if context.case.isa == 'Amd64' else Ia64Evidence).schema(context.target.root)
    record = schema.decode(path.read_bytes())
    if context.case.isa == 'Amd64' and context.case.cpus > 1:
        context.check_problems('session.user-processors.' + name, UserProcessors.problems(
            record.UserProcessors, context.case.cpus, amd64=True, constants=Amd64Layout.headers(context.target.root),
            pool=record.Runtime.PoolPhysicalStart, spinners=context.case.cpus == 4))
        context.check_problems('session.desktop-on-ap.' + name,
                               UserProcessors.session_problems(record.UserProcessors, context.case.cpus))
    return record



def _retained_pairs(context, keys):
    """Immutable image caches survive component teardown."""
    if context.case.isa == 'Amd64' and context.case.cpus == 4:
        keys = (*keys, 'firmware-test:smp_elf')
    return sum(SharedImages.layout(context.target.artifact(key))[0][0] for key in keys)


def _teardown_record(context, name, retained_pairs):
    """Let the pager publish its final management snapshot before freezing.

    IA-64 publishes these counters on entry to Wait. The root's final debug
    line precedes that entry; stopping there can retain the earlier snapshot
    from before the display domain was drained. Keep the exact reclamation
    assertion, and let the next pager service wait publish the completed drain.
    """
    schema = (Amd64Evidence if context.case.isa == 'Amd64' else Ia64Evidence).schema(context.target.root)
    pending = context.artifact(name + '-pending.bin')
    end = time.monotonic() + 30
    while time.monotonic() < end:
        if Evidence.save(context.qmp, context.console.lines, pending) is not None:
            break
        m = schema.decode(pending.read_bytes()).ObjectManagement
        if (m.FramesCreated - m.FramesReclaimed,
                m.MemoryObjectsCreated - m.MemoryObjectsReclaimed) == (retained_pairs, retained_pairs):
            break
        time.sleep(0.02)
    context.stop()
    return _record(context, name)


def _probe_pages(context):
    return Layout.constants((context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text())['ComponentSessionProbePages']


def _addresses(context):
    header = (context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text()
    c = Layout.constants(header)
    prefix = 'Ia64' if context.case.isa == 'Ia64' else ''
    return c[prefix + 'ComponentSessionAddress'], c[prefix + 'ComponentScanoutAddress'], c[prefix + 'ComponentWindowStart']


def _amd64_mapping(context, record):
    address, scanout, producer = _addresses(context)
    c = Amd64Layout.headers(context.target.root)
    o = Amd64Default.Observation.live(context.qmp)
    pool = record.Runtime.PoolPhysicalStart
    roots = [c.root(pool, index) for index in range(4)]
    roots += Amd64Paging.candidate_roots(o.walker,
        [c.pool_page(pool, c.reserved_arena_first_page + slot * c.root_arena_pages)
         for slot in range(c.reserved_arenas)] + c.arena_roots(record.Metadata.ArenaPhysicalStart))
    holders = [root for root in roots if o.walker.walk(root, address)]
    errors = []
    if len(holders) != 1 or not o.walker.walk(holders[0], scanout):
        errors.append('output must be mapped only in the scanout holder: %r' % holders)
    pages = []
    if len(holders) == 1:
        for index in range(_probe_pages(context)):
            leaf = o.walker.walk(holders[0], address + index * 4096)
            if leaf is None or leaf.writable or leaf.executable or not leaf.user or leaf.pat_index:
                errors.append('output page %d is not read-only user WB/NX: %r' % (index, leaf))
            elif not any((other := o.walker.walk(root, producer + index * 4096))
                         and other.physical == leaf.physical and other.writable for root in roots):
                errors.append('output page %d has no writable producer mapping' % index)
            if leaf:
                pages.append({'page': index, 'physical': leaf.physical, 'permissions': leaf.permissions})
    context.artifact('SessionMappings.json').write_text(json.dumps({'holders': holders, 'pages': pages}, indent=2))
    o.save(context.artifact('SessionMappingObservation.json.gz'))
    context.check_problems('session.output-read-only-broker-only', errors)


def _ia64_mapping(context):
    address, scanout, producer = _addresses(context)
    ledgers = _ia64_ledgers(context)
    encoder = Layout.constants((context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp').read_text()
        + (context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.cpp').read_text())
    ppn = encoder['PhysicalPageNumberMask']
    holders = [(at, records) for at, records in ledgers if any(va == address for va, _, _ in records)]
    errors = []
    if len(holders) != 1 or not any(va == scanout for _, records in holders for va, _, _ in records):
        errors.append('output must occur only in the scanout holder: %r' % holders)
    if len(holders) == 1:
        output = [r for r in holders[0][1] if r[0] == address]
        if len(output) != 1 or output[0][2] != _probe_pages(context) or output[0][1] & encoder['UserPrivilegeLevel'] != encoder['UserPrivilegeLevel'] or output[0][1] & (7 << 9):
            errors.append('output ledger is not read-only PL3: %r' % output)
        elif not any(va == producer and pages == _probe_pages(context) and word & ppn == output[0][1] & ppn
                     and word & (7 << 9) == encoder['ReadWriteAccessRights'] for _, records in ledgers for va, word, pages in records):
            errors.append('no matching writable producer ledger')
    context.artifact('SessionMappings.json').write_text(json.dumps(ledgers, indent=2))
    context.check_problems('session.output-read-only-broker-only', errors)


def _ia64_ledgers(context):
    text = Ia64State.read_text(context.qmp)
    context.artifact('SessionRegisters.txt').write_text(text)
    # The pinned runtime reservation's DTR holds every address-space ledger.
    # Their wire layout is asserted in Ia64RuntimeTranslationLedger.hpp;
    # the parser validates publication and capabilities before taking records.
    # The ledger reuses tombstones within the acquired prefix;
    # scan the whole prefix and keep only records with a nonzero page count.
    trs = [entry for entry in Ia64State.parse(text, 0).tlb() if entry.kind == 'DTLB'
           and entry.is_register and entry.ps == Ia64Layout.runtime_memory_page(context.target.root) and entry.pl == 0]
    context.require('session.runtime-dtr', len(trs) == 1, '%r' % trs)
    data = context.qmp.pmemsave(trs[0].pa, trs[0].ps)
    context.artifact('SessionRuntime.bin').write_bytes(data)
    header = (context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp').read_text()
    storage = 'Ia64RuntimeTranslationLedgerWireStorage'
    offsets = Layout.offsets(header, storage)
    size = Layout.size(header, storage)
    stride = Layout.size(header, 'Ia64RuntimeTranslationLedgerWireRecord')
    maximum = Layout.constants((context.target.root / 'Source/Kernel/Memory/AddressSpaceMappingRecord.hpp').read_text())['MaximumRuntimeAddressSpaceMappings']
    encoder = Layout.constants((context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp').read_text()
        + (context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.cpp').read_text())
    present, ppn = encoder['PresentBit'], encoder['PhysicalPageNumberMask']
    ledgers = []
    for at in range(0, len(data) - size, 8):
        mask, physical, virtual, rid, key, attributes, walker, reserved, reserved64 = struct.unpack_from('<QBBBBBBHQ', data, at)
        if (not mask & encoder['FourKibibytePageSizeBit'] or not 32 <= physical <= 63 or not 31 <= virtual <= 60
                or not 8 <= rid <= 24 or not 8 <= key <= 24 or not attributes & 1
                or walker > 1 or reserved or reserved64):
            continue
        published, = struct.unpack_from('<Q', data, at + offsets['PublicationWord'])
        if not 1 <= published <= maximum + 1:
            continue
        records = [struct.unpack_from('<QQQ', data, at + offsets['Records'] + index * stride) for index in range(published - 1)]
        if any(pages and (va & 4095 or not word & present or pages > 8192) for va, word, pages in records):
            continue
        ledgers.append((at, [r for r in records if r[2]]))
    return ledgers


def run(context):
    phases = (('pattern 1', 1), ('pattern 2', 2), ('console restored', None),
              ('shown again', 2), ('dropped', None), ('cleared trusted console', None))
    since = 0
    for phase, serial in phases:
        index, _ = context.wait_for_line(PREFIX + phase, 90, since=since)
        fence, _ = context.wait_for_line('USER:# pointer:', 30, since=index + 1)
        since = fence + 1
        context.stop()
        try:
            path = context.artifact(phase.replace(' ', '-') + '.ppm')
            context.qmp.screendump(path, format='ppm')
            width, height, actual = Display.parse_ppm(path.read_bytes())
            if serial is None:
                lines = Guest.root_service_lines(context.console.lines)
                errors = Display.display_problems(path.read_bytes(), lines, context.target.root,
                                                  isa=context.case.isa)
            else:
                expected = Session.probe_output(context.target.root, width, height, serial)
                context.artifact(phase.replace(' ', '-') + '-expected.ppm').write_bytes(Display.write_ppm(width, height, expected))
                different = sum(actual[at:at + 3] != expected[at:at + 3] for at in range(0, len(actual), 3))
                errors = [] if not different else ['%d pixels differ' % different]
            context.check_problems('session.screen.' + phase.replace(' ', '-'), errors)
            if phase == 'pattern 1':
                record = _record(context, 'SessionActiveEvidence')
                if context.case.isa == 'Amd64':
                    _amd64_mapping(context, record)
                else:
                    _ia64_mapping(context)
        finally:
            context.qmp.cont()
    context.wait_for_line('USER:# root service: done', 90, since=since)
    Guest.wait(context)
    time.sleep(0.2)
    context.stop()
    record = _record(context, 'SessionTeardownEvidence')
    retained = sum(SharedImages.layout(context.target.artifact(key))[0][0]
                   for key in ('firmware:display_elf', 'firmware-test:session_probe_elf'))
    if context.case.isa == 'Amd64' and context.case.cpus == 4:
        retained += SharedImages.layout(context.target.artifact('firmware-test:smp_elf'))[0][0]
    management = record.ObjectManagement
    frames = management.FramesCreated - management.FramesReclaimed
    memories = management.MemoryObjectsCreated - management.MemoryObjectsReclaimed
    context.check('session.teardown-counts', (frames, memories) == (retained, retained),
                  'live Frames/MemoryObjects %d/%d; retained image segments %d' % (frames, memories, retained))
    context.check('pager.frame-rights', Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(context.console.lines))
    context.check('session.holder-stamp', PREFIX + 'holder snapshot 96,72 sequence 2 stamp 42' in context.console.lines)
    context.check('session.metrics', any(line.startswith('USER:# display: scanout ') and ' us, session ' in line
                                        for line in context.console.lines))
    context.check('session.pointer-stamp', PREFIX + 'pointer 96,72 sequence 1 stamp 42' in context.console.lines)
