# SPDX-License-Identifier: Apache-2.0
"""desktop session: fenced scene transcripts and creator-domain drains."""
import json
from AsteriaVm import Display, Guest, Session
from AsteriaVerify import Layout
from AsteriaVm import Amd64Layout
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import Amd64Paging
from AsteriaVm.Observers import SharedImages
from AsteriaVm.Drivers.SessionProbe import _record, _teardown_record, _retained_pairs, _ia64_ledgers

PREFIX = 'USER:# session-test: '


def complete_snapshot(context, fence):
    """A display pointer fence can precede the last diagnostic write.

    desktop session: wait for the matching Presented metadata before pausing;
    never replay a partial snapshot or replace the fence with a timing delay.
    """
    pending = None
    for index, line in enumerate(context.console.lines[:fence + 1]):
        if line.startswith('USER:# session: begin '):
            pending = index, line.split()[3]
        elif line.startswith('USER:# session: presented '):
            pending = None
    if pending is not None:
        index, serial = pending
        presented, _ = context.wait_for_line('USER:# session: presented ' + serial + ' ', 30, since=index + 1)
        fence = max(fence, presented)
    return fence + 1


def mappings(context, record, width, height, dropped=False):
    constants = Layout.constants((context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text())
    prefix = 'Ia64' if context.case.isa == 'Ia64' else ''
    base = constants[prefix + 'ComponentCompositorAddress']
    address = Layout.constants((context.target.root / 'Source/User/Session/SessionFrame.hpp').read_text()
        + (context.target.root / 'Source/User/Session/AddressPlan.hpp').read_text())
    output = base + address['AddressOutputOffset']
    fonts = base + address['AddressUiFontOffset']
    pools = [base, base + address['AddressClientSpan']]
    broker = constants[prefix + 'ComponentSessionAddress']
    producer = constants[prefix + 'ComponentWindowStart']
    # Ordinary clients reserve native-sized returned images, even when this composition test
    # explicitly uses a small output.
    image_pages = (width * height * 4 + 4095) // 4096
    session = Layout.constants((context.target.root / 'Source/User/Session/SessionFrame.hpp').read_text())
    pool_pages = image_pages if 2 * image_pages > session['SessionMaximumPoolPages'] else 2 * image_pages
    errors = []
    if context.case.isa == 'Amd64':
        c = Amd64Layout.headers(context.target.root)
        observation = Amd64Default.Observation.live(context.qmp)
        physical = record.Runtime.PoolPhysicalStart
        roots = [c.root(physical, index) for index in range(4)]
        roots += Amd64Paging.candidate_roots(observation.walker,
            [c.pool_page(physical, c.reserved_arena_first_page + slot * c.root_arena_pages)
             for slot in range(c.reserved_arenas)] + c.arena_roots(record.Metadata.ArenaPhysicalStart))
        layout = Layout.constants((context.target.root / 'Source/Abi/ObjectCreationLimits.hpp').read_text()
                                  + (context.target.root / 'Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp').read_text())
        roots += Amd64Paging.candidate_roots(observation.walker,
            [c.pool_page(physical, layout['LargeRootArenaFirstPage'] + slot * layout['LargeRootArenaPages'])
             for slot in range(layout['LargePageTableArenaMaximumCount'])])
        composers = [root for root in roots if observation.walker.walk(root, output)]
        brokers = [root for root in roots if observation.walker.walk(root, broker)]
        if len(composers) != 1 or len(brokers) != 1 or composers == brokers:
            errors.append('expected separate compositor and broker roots')
        else:
            for index in range(constants['ComponentCompositorPages']):
                own = observation.walker.walk(composers[0], output + index * 4096)
                borrowed = observation.walker.walk(brokers[0], broker + index * 4096)
                if not own or not borrowed or not own.writable or borrowed.writable or not borrowed.user or borrowed.executable or own.physical != borrowed.physical:
                    errors.append('output permissions/sharing page %d' % index)
            for index in range(constants['ComponentUiFontPages']):
                leaf = observation.walker.walk(composers[0], fonts + index * 4096)
                if not leaf or leaf.writable or not leaf.user or leaf.executable or leaf.pat_index:
                    errors.append('font data is not read-only User WB/NX')
                if observation.walker.walk(brokers[0], fonts + index * 4096):
                    errors.append('broker maps font data')
            for slot in range(2):
                pages = pool_pages
                for index in range(pages):
                    own = observation.walker.walk(composers[0], pools[slot] + index * 4096)
                    if dropped and slot == 1:
                        if own: errors.append('dead pool still mapped')
                        continue
                    if not own or own.writable or not own.user or own.executable or own.pat_index:
                        errors.append('client pool is not read-only User WB/NX')
                    elif not any((leaf := observation.walker.walk(root, producer + index * 4096))
                                 and leaf.writable and leaf.physical == own.physical for root in roots):
                        errors.append('pool has no matching writable producer')
                    if observation.walker.walk(brokers[0], pools[slot] + index * 4096):
                        errors.append('broker maps client memory')
        observation.save(context.artifact('DroppedMappings.json.gz' if dropped else 'SceneMappings.json.gz'))
    else:
        ledgers = _ia64_ledgers(context)
        composers = [records for _, records in ledgers if any(va == output for va, _, _ in records)]
        brokers = [records for _, records in ledgers if any(va == broker for va, _, _ in records)]
        context.artifact('DroppedMappings.json' if dropped else 'SceneMappings.json').write_text(json.dumps(ledgers, indent=2))
        # Ordinary PL3 4 KiB mappings. The encoding is taken from this project's encoder, as in the
        # ledger observer.
        encoder = Layout.constants((context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp').read_text()
            + (context.target.root / 'Source/Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.cpp').read_text())
        mask = encoder['PhysicalPageNumberMask']
        def find(records, address):
            return next((r for r in records if r[0] == address), None)
        if len(composers) != 1 or len(brokers) != 1:
            errors.append('expected separate compositor and broker ledgers')
        else:
            own, borrowed = find(composers[0], output), find(brokers[0], broker)
            if (own[1] & mask != borrowed[1] & mask or borrowed[1] & (7 << 9)
                    or borrowed[1] & encoder['UserPrivilegeLevel'] != encoder['UserPrivilegeLevel']
                    or own[1] & (7 << 9) != encoder['ReadWriteAccessRights']
                    or own[2] != constants['ComponentCompositorPages'] or borrowed[2] != own[2]):
                errors.append('output is not shared read-only')
            font = find(composers[0], fonts)
            if not font or font[2] != constants['ComponentUiFontPages'] or font[1] & (7 << 9) or (font[1] >> 2) & 7 != encoder['WriteBackMemoryAttribute'] or font[1] & encoder['UserPrivilegeLevel'] != encoder['UserPrivilegeLevel']:
                errors.append('font data is not read-only PL3 WB/NX')
            if find(brokers[0], fonts): errors.append('broker maps font data')
            for slot in range(2):
                for offset in range(0, pool_pages, session['SessionMaximumFramePages']):
                    pages = min(pool_pages - offset, session['SessionMaximumFramePages'])
                    own = find(composers[0], pools[slot] + offset * 4096)
                    if dropped and slot == 1:
                        if own: errors.append('dead pool still mapped')
                        continue
                    if not own or own[2] != pages or own[1] & (7 << 9) or own[1] & encoder['UserPrivilegeLevel'] != encoder['UserPrivilegeLevel']:
                        errors.append('client pool is not read-only PL3')
                    elif not any(va == producer + offset * 4096 and word & mask == own[1] & mask
                                 and word & (7 << 9) == encoder['ReadWriteAccessRights']
                                 for _, records in ledgers for va, word, count in records if count == pages):
                        errors.append('pool has no matching writable producer')
                    if find(brokers[0], pools[slot] + offset * 4096): errors.append('broker maps client memory')
    context.check_problems('session.mappings.' + ('dropped' if dropped else 'active'), errors)


def run(context):
    since = 0
    for phase in ('stacked', 'damage', 'clipped', 'client killed, last image retained', 'client dropped', 'surface destroyed', 'compositor detached'):
        index, _ = context.wait_for_line(PREFIX + phase, 90, since=since)
        fence, line = context.wait_for_line('USER:# pointer:', 30, since=index + 1)
        since = complete_snapshot(context, fence)
        context.stop()
        try:
            path = context.artifact(phase.replace(' ', '-') + '.ppm')
            context.qmp.screendump(path, format='ppm')
            width, height, actual = Display.parse_ppm(path.read_bytes())
            lines = Guest.root_service_lines(context.console.lines[:since])
            cursor = Display.pointer_position(lines, width, height)
            expected = Session.transcript_output(context.target.root, width, height, lines, cursor)
            context.artifact(phase.replace(' ', '-') + '-expected.ppm').write_bytes(Display.write_ppm(width, height, expected))
            different = sum(actual[at:at + 3] != expected[at:at + 3] for at in range(0, len(actual), 3))
            context.check('session.scene.' + phase.replace(' ', '-'), not different, '%d pixels differ' % different)
            record = _record(context, 'Session-' + phase.replace(' ', '-'))
            if phase == 'stacked':
                from AsteriaVm.Drivers.SessionInput import windows
                scene = windows(lines)
                context.check('session.stale-launch', scene[0][4] == 1 and scene[4][4] == 2, str(scene))
                expected_font = '# session: font ' + ('Bold8x16-fallback Bold8x16-fallback' if context.case.ui_font_absent else 'Vegur-Bold Vegur-Regular')
                context.check('session.ui-font', expected_font in lines, expected_font)
                context.check('session.ui-font-write-refused', '# session: font-map read-only Map write-refused' in lines)
                baseline = record
                mappings(context, record, width, height)
            if phase == 'client dropped':
                # Domain charge includes client-owned pools and private data.
                mappings(context, record, width, height, dropped=True)
                context.check('session.drop-unmaps', record.ObjectManagement.Unmappings > baseline.ObjectManagement.Unmappings)
                resource = record.ObjectManagement
                before = baseline.ObjectManagement
                constants = Layout.constants((context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text())
                stack = constants[('Ia64' if context.case.isa == 'Ia64' else '') + 'ComponentStackPages'] + 1
                expected_charge = 4 * ((width * height * 4 + 4095) // 4096) + stack + SharedImages.private_pages(context.target.artifact('firmware-test:pattern_elf'))
                delta = (before.CommittedPages - resource.CommittedPages, before.ResidentPages - resource.ResidentPages)
                context.check('session.client-charge-returned', delta == (expected_charge, expected_charge),
                              '%r vs %d pages' % (delta, expected_charge))
        finally:
            context.qmp.cont()
    context.wait_for_line('USER:# root service: done', 90, since=since)
    Guest.wait(context)
    retained_pairs = _retained_pairs(context,
        ('firmware:display_elf', 'firmware-test:compositor_elf', 'firmware-test:pattern_elf'))
    record = _teardown_record(context, 'SessionTeardownEvidence', retained_pairs)
    management = record.ObjectManagement
    live = (management.FramesCreated - management.FramesReclaimed,
            management.MemoryObjectsCreated - management.MemoryObjectsReclaimed)
    context.check('session.teardown-counts', live == (retained_pairs, retained_pairs), '%r vs %d' % (live, retained_pairs))
    context.check('session.killed-client-keeps-image', PREFIX + 'client killed, last image retained' in context.console.lines)
    context.check('session.pool-and-range', PREFIX + 'bad range refused, 16 Frame attachments in four messages per client' in context.console.lines)
    context.check('session.pool-write-refused', PREFIX + 'client pool writable mapping refused' in context.console.lines)
