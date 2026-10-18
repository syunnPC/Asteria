# SPDX-License-Identifier: Apache-2.0
"""Stop vocabulary, terminal lifecycle, and screen model without VM fixtures."""
import pathlib
import tempfile
import unittest

from AsteriaVm import Display, Evidence, Stops
from AsteriaVm.Observers import Amd64Evidence, Ia64Evidence

ROOT = pathlib.Path(__file__).resolve().parents[3]


def evidence(isa="Amd64"):
    schema = (Ia64Evidence if isa == "Ia64" else Amd64Evidence).schema(ROOT)
    data = bytearray(schema.record.size)
    import struct
    struct.pack_into('<QII', data, 0, schema.magic, schema.version, len(data))
    record = schema.decode(bytes(data))
    stops = record.Stops
    stops.Published = stops.DrawPublished = 1
    stops.DrawStates = [2] * 4
    stops.OnlineMask = stops.ConfirmedMask = 15
    stops.Winner = 0
    stops.Processors[0].Stop.Code = Stops.vocabulary(ROOT)['TestInjected']
    stops.Processors[0].Stop.Parameters[0] = 1
    for cpu in range(4):
        stops.Processors[cpu].Stop.State = 2
    return stops


def line(stops):
    code = stops.Processors[0].Stop.Code
    return 'ASTERIA:STOP:CPU=0:CODE=0x%016X:TestInjected:P=0x0000000000000001,%s,%s,%s' % (
        code, *(['0x0000000000000000'] * 3))


class StopTests(unittest.TestCase):
    def test_vocabulary_and_current_evidence_layout(self):
        names = Stops.vocabulary(ROOT)
        self.assertEqual(len(names), 11)
        self.assertEqual(names['Lockup'], 11)
        self.assertNotIn('ProcessorLost', names)
        self.assertEqual(Amd64Evidence.schema(ROOT).record.field('Stops').element.size, 592)

    def test_watchdog_labels_and_location(self):
        schema = Amd64Evidence.schema(ROOT)
        import struct
        data = bytearray(schema.record.size)
        struct.pack_into('<QII', data, 0, schema.magic, schema.version, len(data))
        watchdog = schema.decode(bytes(data)).Watchdog
        watchdog.StalledMask = 1
        sample = watchdog.Processors[0]
        sample.AgeMilliseconds = 5250
        sample.Component = 13  # ComponentBehaviorDisplay; the ABI's stable kind.
        sample.Location = 0x1234
        sample.Captured = 1
        text = Display.stop_text(evidence(), 4, ROOT, watchdog=watchdog, text_bounds=(0x1000, 0x2000))
        self.assertIn('CPU 0 frozen confirmed stalled age 5250 ms display-broker', text)
        self.assertIn('captured 0x0000000000001234 KERNEL.ELF .text+0x0000000000000234', text)
        sample.Captured = 0
        self.assertIn('last heartbeat', Display.stop_text(evidence(), 4, ROOT, watchdog=watchdog))

    def test_watchdog_label_formatting(self):
        headers = (
            'case 0U: return "idle/kernel"; case ComponentBehaviorDisplay: return "display-broker"; '
            'default: return "component";',
            'case 0U:\n    return "idle/kernel";\ncase ComponentBehaviorDisplay:\n'
            '    return "display-broker";\ndefault:\n    return "component";',
            'case\t0U\t:\nreturn\n"idle/kernel" ;\ncase\nComponentBehaviorDisplay\n:\n'
            'return\t"display-broker"\n;\ndefault\t:\nreturn\n"component"\t;',
        )
        with tempfile.TemporaryDirectory() as temporary:
            repository = pathlib.Path(temporary)
            abi = repository / 'Source/Abi'
            abi.mkdir(parents=True)
            (abi / 'ComponentBootstrap.hpp').write_text('constexpr UInt64 ComponentBehaviorDisplay{13U};')
            for header in headers:
                with self.subTest(header=header):
                    (abi / 'Watchdog.hpp').write_text(header)
                    self.assertEqual(Display.watchdog_component_name(repository, 0), 'idle/kernel')
                    self.assertEqual(Display.watchdog_component_name(repository, 13), 'display-broker')
                    self.assertEqual(Display.watchdog_component_name(repository, 999), 'component')

    def test_watchdog_missing_default_label(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = pathlib.Path(temporary)
            abi = repository / 'Source/Abi'
            abi.mkdir(parents=True)
            (abi / 'ComponentBootstrap.hpp').write_text('')
            (abi / 'Watchdog.hpp').write_text('case 0U:\n    return "idle/kernel";')
            with self.assertRaisesRegex(Display.DisplayError, 'Watchdog.hpp: WatchdogComponentName default label missing'):
                Display.watchdog_component_name(repository, 999)

    def test_lifecycle_mutants(self):
        stops = evidence()
        good = line(stops)
        self.assertEqual(Stops.problems([good], 'TestInjected', 4, ROOT, stops), [])
        for lines in ([], [good, good], [good.replace('TestInjected', 'UnexpectedNmi')],
                      [good.replace('CPU=0', 'CPU=1')], [good.replace('0001', '0002')]):
            self.assertTrue(Stops.problems(lines, 'TestInjected', 4, ROOT, stops))
        stops.Processors[3].Stop.State = 0
        self.assertTrue(Stops.problems([good], 'TestInjected', 4, ROOT, stops))
        stops.ConfirmedMask = 7
        stops.UnconfirmedMask = 8
        self.assertEqual(Stops.problems([good], 'TestInjected', 4, ROOT, stops), [])
        stops.ConfirmedMask = 15
        self.assertTrue(Stops.problems([good], 'TestInjected', 4, ROOT, stops))

    def test_early_stop_and_retained_draw_state(self):
        stops = evidence()
        stops.OnlineMask = stops.ConfirmedMask = 1
        for cpu in range(1, 4):
            stops.Processors[cpu].Stop.State = 0
            stops.DrawStates[cpu] = 0
        self.assertEqual(Stops.problems([line(stops)], 'TestInjected', 4, ROOT, stops, 1), [])
        self.assertIn('CPU 3 running not online', Display.stop_text(stops, 4, ROOT))
        stops.Processors[3].Stop.State = 2
        self.assertIn('CPU 3 running not online', Display.stop_text(stops, 4, ROOT))
        stops.Processors[0].Stop.Code = Stops.vocabulary(ROOT)['BootStage']
        stops.Processors[0].Stop.Parameters[0] = 10
        self.assertIn('P1 0x000000000000000A Processors', Display.stop_text(stops, 4, ROOT))

    def test_screen_and_identity_mask(self):
        stops = evidence()
        text = Display.stop_text(stops, 4, ROOT)
        self.assertIn('Code 0x00000006 TestInjected', text)
        self.assertIn('CPU 3 frozen confirmed', text)
        self.assertIn('DMA not quiesced', text)
        console = Display.TextConsole(1280, 800)
        console.write(text.encode('ascii'))
        pixels = console.render(1280, 800, Display.load_font(ROOT), ((255, 255, 255), (0, 140, 166)))
        ppm = Display.write_ppm(1280, 800, pixels)
        self.assertEqual(Display.stop_display_problems(ppm, stops, ROOT, 4), [])
        changed = bytearray(pixels)
        changed[0:3] = bytes((1, 2, 3))
        self.assertTrue(Display.stop_display_problems(Display.write_ppm(1280, 800, changed), stops, ROOT, 4))
        changed = bytearray(pixels)
        changed[17 * 16 * 1280 * 3:17 * 16 * 1280 * 3 + 3] = bytes((1, 2, 3))
        self.assertEqual(Display.stop_display_problems(Display.write_ppm(1280, 800, changed), stops, ROOT, 4), [])


class Ia64StopTests(unittest.TestCase):
    def test_layout_and_snapshot_rendering(self):
        schema = Ia64Evidence.schema(ROOT)
        self.assertEqual(schema.version, 32)
        self.assertEqual(schema.record.size, 19256)
        self.assertEqual(schema.record.field('Stops').element.size, 624)
        stops = evidence('Ia64')
        stops.Processors[0].Snapshot.Iip = 0x12345678
        text = Display.stop_text(stops, 4, ROOT, 'Ia64')
        self.assertIn('IIP 0x0000000012345678', text)
        self.assertIn('RID 0x0000000000000000', text)
        self.assertIn('Uptime ITC', text)
        stops.Processors[0].Stop.Code = Stops.vocabulary(ROOT)['ApplicationProcessorStage']
        stops.Processors[0].Stop.Parameters[0] = 2
        self.assertIn('ShootdownDrain', Display.stop_text(stops, 4, ROOT, 'Ia64'))

    def test_unconfirmed_peer_and_scanout_mutants(self):
        stops = evidence('Ia64')
        stops.ConfirmedMask = 7
        stops.UnconfirmedMask = 8
        stops.Processors[3].Stop.State = 3
        stops.DrawStates[3] = 3
        self.assertEqual(Stops.problems([line(stops)], 'TestInjected', 4, ROOT, stops), [])
        text = Display.stop_text(stops, 4, ROOT, 'Ia64')
        self.assertIn('CPU 3 unconfirmed unconfirmed', text)
        console = Display.TextConsole(640, 400)
        console.write(text.encode('ascii'))
        pixels = console.render(640, 400, Display.load_font(ROOT), ((255,255,255), (0,140,166)))
        ppm = Display.write_ppm(640, 400, pixels)
        self.assertEqual(Display.stop_display_problems(ppm, stops, ROOT, 4, 'Ia64'), [])
        changed = bytearray(pixels)
        changed[0:3] = bytes((1,2,3))
        self.assertTrue(Display.stop_display_problems(Display.write_ppm(640,400,changed), stops, ROOT,4,'Ia64'))
