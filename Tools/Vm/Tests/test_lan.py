# SPDX-License-Identifier: Apache-2.0
"""RFC byte vectors and real socketpair LAN demonstrations; no VM or guest C++."""
from __future__ import annotations

import dataclasses
import json
import pathlib
import socket
import struct
import tempfile
import threading
import time
import unittest

from AsteriaVm import Guest, Lan

# Synthetic test inputs with project-chosen addresses and payloads. These
# literal vectors and their checksums are authored independently of Lan.py.
ARP_VECTOR = bytes.fromhex('ffffffffffff02000000000208060001080006040001020000000002c000020a000000000000c0000214000000000000000000000000000000000000')
ICMP_VECTOR = bytes.fromhex('020000000001020000000002080045000054135700004001e333c000020ac000021408004d74246800074173746572696120636c65616e20726f6f6d206563686f20000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f')
UDP_VECTOR = bytes.fromhex('020000000001020000000002080045000053135700004011e324c000020ac00002149c400007003f97054173746572696120636c65616e20726f6f6d2075647020000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f')


def guest_udp(payload=Lan.ECHO_PAYLOAD, destination=Lan.PEER_IP, *, bad_checksum=False):
    packet = bytearray(Lan.udp(Lan.GUEST_IP, destination, 7, 40000, payload))
    if bad_checksum:
        packet[6] ^= 1
    return Lan.ethernet(Lan.PEER_MAC, Lan.GUEST_MAC, 0x800,
                        Lan.ipv4(Lan.GUEST_IP, destination, 17, bytes(packet)))


def guest_echo(identifier, sequence, payload, destination=Lan.PEER_IP):
    return Lan.ethernet(Lan.PEER_MAC, Lan.GUEST_MAC, 0x800,
                        Lan.ipv4(Lan.GUEST_IP, destination, 1,
                                 Lan.icmp_echo(identifier, sequence, payload, reply=True)))


def failures(results):
    return {name: '\n'.join(problems) for name, problems in results.items() if problems}


class WireTests(unittest.TestCase):
    def test_rfc1071_numerical_example_and_odd_length(self):
        self.assertEqual(Lan.checksum(bytes.fromhex('0001f203f4f5f6f7')), 0x220D)
        self.assertEqual(Lan.checksum(bytes.fromhex('0001f2')), 0x0DFE)
        self.assertEqual(Lan.checksum(b''), 0xFFFF)
        self.assertEqual(Lan.checksum(b'\xff' * 100001), 0x00FF)

    def test_synthetic_arp_byte_exact(self):
        self.assertEqual(Lan.arp_request('02:00:00:00:00:02', '192.0.2.10', '192.0.2.20'), ARP_VECTOR)
        frame = Lan.decode_frame(ARP_VECTOR)
        self.assertEqual(frame['arp'], dict(opcode=1, sender_mac='02:00:00:00:00:02',
            sender_ip='192.0.2.10', target_mac=Lan.ZERO_MAC, target_ip='192.0.2.20'))
        reply = bytes.fromhex('525400a57e02525400a57e0108060001080006040002'
                              '525400a57e010a00020f525400a57e020a000202') + bytes(18)
        self.assertEqual(Lan.arp_reply(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.PEER_MAC, Lan.PEER_IP), reply)
        self.assertEqual(Lan.decode_frame(reply)['arp']['opcode'], 2)

    def test_synthetic_icmp_byte_exact(self):
        payload = b'Asteria clean room echo ' + bytes(range(32))
        icmp = Lan.icmp_echo(0x2468, 7, payload)
        frame = Lan.ethernet('02:00:00:00:00:01', '02:00:00:00:00:02', 0x800,
                             Lan.ipv4('192.0.2.10', '192.0.2.20', 1, icmp, identification=0x1357))
        self.assertEqual(frame, ICMP_VECTOR)
        self.assertEqual(Lan.parse_icmp(ICMP_VECTOR[34:])['payload'], payload.hex())
        reply = Lan.icmp_echo(0x2468, 7, payload, reply=True)
        self.assertEqual(reply[:8], bytes.fromhex('0000557424680007'))
        self.assertEqual(Lan.parse_icmp(reply)['type'], 0)

    def test_synthetic_udp_byte_exact(self):
        payload = b'Asteria clean room udp ' + bytes(range(32))
        datagram = Lan.udp('192.0.2.10', '192.0.2.20', 40000, 7, payload)
        frame = Lan.ethernet('02:00:00:00:00:01', '02:00:00:00:00:02', 0x800,
                             Lan.ipv4('192.0.2.10', '192.0.2.20', 17, datagram, identification=0x1357))
        self.assertEqual(frame, UDP_VECTOR)
        decoded = Lan.decode_frame(UDP_VECTOR)
        self.assertEqual(decoded['ipv4']['udp']['checksum'], 0x9705)
        self.assertTrue(decoded['ipv4']['udp']['checksum_valid'])

    def test_udp_zero_checksum_and_computed_zero(self):
        packet = Lan.udp(Lan.PEER_IP, Lan.GUEST_IP, 40000, 7, b'odd', with_checksum=False)
        self.assertEqual(packet[6:8], b'\0\0')
        self.assertTrue(Lan.parse_udp(packet, Lan.PEER_IP, Lan.GUEST_IP)['checksum_valid'])
        # Pseudo-header + zeroed header sums to 0x0025; 0xffda makes
        # a one's-complement sum of 0xffff. Calculated zero goes on wire as ffff.
        packet = Lan.udp('0.0.0.0', '0.0.0.0', 0, 0, bytes.fromhex('ffda'))
        self.assertEqual(packet[6:8], b'\xff\xff')
        self.assertTrue(Lan.parse_udp(packet, '0.0.0.0', '0.0.0.0')['checksum_valid'])

    def test_ethernet_padding_does_not_enter_ip_or_transport_checksum(self):
        frame = guest_udp(b'x')
        self.assertEqual(len(frame), 60)
        self.assertEqual(frame[-17:], bytes(17))
        self.assertEqual(Lan.decode_frame(frame)['ipv4']['udp']['payload'], '78')
        # Padding is outside IP total length and may contain arbitrary bytes.
        self.assertEqual(Lan.decode_frame(frame[:-17] + b'\xff' * 17), Lan.decode_frame(frame))
        self.assertEqual(Lan.pad_frame(bytes(80)), bytes(80))

    def test_parser_rejections(self):
        for size in range(14):
            with self.subTest(ethernet_size=size), self.assertRaises(ValueError):
                Lan.parse_ethernet(ARP_VECTOR[:size])
        for size in range(28):
            with self.subTest(arp_size=size), self.assertRaises(ValueError):
                Lan.parse_arp(ARP_VECTOR[14:14 + size])
        for parser, packet, offsets in (
            (Lan.parse_arp, ARP_VECTOR[14:], (0, 2, 4, 5, 7)),
            (Lan.parse_ipv4, ICMP_VECTOR[14:], (0, 2, 6, 10)),
            (Lan.parse_icmp, ICMP_VECTOR[34:], (0, 1, 2)),
            (lambda p: Lan.parse_udp(p, '192.0.2.10', '192.0.2.20'), UDP_VECTOR[34:], (4, 6))):
            for offset in offsets:
                corrupt = bytearray(packet)
                corrupt[offset] ^= 1
                with self.subTest(parser=parser, offset=offset), self.assertRaises(ValueError):
                    parser(bytes(corrupt))
        for parser, packet, length in ((Lan.parse_ipv4, ICMP_VECTOR[14:], 20),
                                      (Lan.parse_icmp, ICMP_VECTOR[34:], 8),
                                      (lambda p: Lan.parse_udp(p, '192.0.2.10', '192.0.2.20'),
                                       UDP_VECTOR[34:], 8)):
            for size in range(length):
                with self.subTest(size=size), self.assertRaises(ValueError):
                    parser(packet[:size])
        with self.assertRaises(ValueError):
            Lan.parse_ipv4(ICMP_VECTOR[14:-1])
        with self.assertRaises(ValueError):
            Lan.parse_udp(UDP_VECTOR[34:], '192.0.2.11', '192.0.2.20')
        with self.assertRaises(ValueError):
            Lan.parse_ethernet(bytes(14))
        with self.assertRaises(ValueError):
            Lan.ethernet(Lan.PEER_MAC, Lan.GUEST_MAC, 100, b'')
        with self.assertRaises(ValueError):
            Lan.arp(3, Lan.PEER_MAC, Lan.PEER_IP, Lan.GUEST_MAC, Lan.GUEST_IP)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = pathlib.Path(self.directory.name)
        self.frames = [Lan.Frame('peer', 1_700_000_000_123456, ARP_VECTOR),
                       Lan.Frame('guest', 1_700_000_001_000001, ICMP_VECTOR)]

    def test_pcap_round_trip_and_literal_header(self):
        Lan.save_capture(self.path, self.frames)
        data = (self.path / Lan.PCAP_ARTIFACT).read_bytes()
        self.assertEqual(data[:24], bytes.fromhex('d4c3b2a1020004000000000000000000ffff000001000000'))
        self.assertEqual(data[24:40], struct.pack('<IIII', 1700000000, 123456, 60, 60))
        self.assertEqual(Lan.load_capture(self.path), (self.frames, []))
        rows = json.loads((self.path / Lan.JSON_ARTIFACT).read_text())['frames']
        self.assertEqual(rows[0]['direction'], 'peer')
        self.assertEqual(rows[0]['summary']['arp']['target_ip'], '192.0.2.20')
        self.assertEqual(rows[0]['time_us'], self.frames[0].time_us)

    def test_big_endian_pcap(self):
        frame = self.frames[0]
        sec, usec = divmod(frame.time_us, 1000000)
        data = (struct.pack('>IHHIIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
                + struct.pack('>IIII', sec, usec, len(frame.data), len(frame.data)) + frame.data)
        path = self.path / Lan.PCAP_ARTIFACT
        path.write_bytes(data)
        self.assertEqual(Lan.read_pcap(path), [(frame.time_us, frame.data)])

    def test_reject_truncated_or_unsupported_pcap(self):
        Lan.save_capture(self.path, self.frames)
        path = self.path / Lan.PCAP_ARTIFACT
        original = path.read_bytes()
        mutants = [original[:n] for n in (0, 23, 25, 39, len(original) - 1)]
        for offset, data in ((0, b'bad!'), (20, struct.pack('<I', 101)),
                             (28, struct.pack('<I', 1000000)), (36, struct.pack('<I', 59))):
            mutants.append(original[:offset] + data + original[offset + len(data):])
        for data in mutants:
            path.write_bytes(data)
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                Lan.read_pcap(path)

    def test_missing_or_inconsistent_artifacts_fail_every_check(self):
        for filename in (Lan.PCAP_ARTIFACT, Lan.JSON_ARTIFACT):
            Lan.save_capture(self.path, self.frames)
            (self.path / filename).unlink()
            failed = failures(Lan.judge_artifacts(self.path, []))
            self.assertEqual(set(failed), set(Lan.CHECKS))
            self.assertTrue(all('missing LAN artifact: ' + filename in detail for detail in failed.values()))
        Lan.save_capture(self.path, self.frames)
        path = self.path / Lan.JSON_ARTIFACT
        original = json.loads(path.read_text())
        for field, value in (('time_us', 0), ('summary', {}), ('direction', 'invalid')):
            record = json.loads(json.dumps(original))
            record['frames'][0][field] = value
            path.write_text(json.dumps(record))
            self.assertTrue(all('mismatch' in d for d in failures(Lan.judge_artifacts(self.path, [])).values()))
        path.write_text('{invalid')
        self.assertEqual(set(failures(Lan.judge_artifacts(self.path, []))), set(Lan.CHECKS))

    def test_empty_capture_does_not_pass_negative_check(self):
        self.assertEqual(set(failures(Lan.judge_frames([], ['USER:' + Lan.DROPS_LINE]))), set(Lan.CHECKS))


class ReplayTests(unittest.TestCase):
    def setUp(self):
        # Model a complete capture, separately from the socketpair tests, to
        # isolate replay invariants and corruption of otherwise valid frames.
        bad = bytearray(Lan.icmp_echo(*Lan.BAD_ECHO))
        bad[2] ^= 1
        self.frames = []
        def add(direction, data):
            self.frames.append(Lan.Frame(direction, 1700000000000000 + len(self.frames) * 600000, data))
        add('guest', Lan.arp_request(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.PEER_IP))
        add('peer', Lan.arp_reply(Lan.PEER_MAC, Lan.PEER_IP, Lan.GUEST_MAC, Lan.GUEST_IP))
        add('guest', guest_udp(Lan.RESOLVE_PAYLOAD))
        add('peer', Lan.arp_request(Lan.PEER_MAC, Lan.PEER_IP, Lan.OTHER_IP))
        add('peer', Lan.arp_request(Lan.PEER_MAC, Lan.PEER_IP, Lan.GUEST_IP))
        add('guest', Lan.arp_reply(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.PEER_MAC, Lan.PEER_IP))
        add('peer', Lan.ethernet(Lan.GUEST_MAC, Lan.PEER_MAC, 0x800,
                               Lan.ipv4(Lan.PEER_IP, Lan.GUEST_IP, 1, bytes(bad))))
        add('peer', Lan.ethernet(Lan.GUEST_MAC, Lan.PEER_MAC, 0x800,
                               Lan.ipv4(Lan.PEER_IP, Lan.GUEST_IP, 1, Lan.icmp_echo(*Lan.GOOD_ECHO))))
        add('guest', guest_echo(*Lan.GOOD_ECHO))
        add('peer', Lan.ethernet(Lan.GUEST_MAC, Lan.PEER_MAC, 0x800,
                               Lan.ipv4(Lan.UNAUTHORIZED_IP, Lan.GUEST_IP, 17,
                                        Lan.udp(Lan.UNAUTHORIZED_IP, Lan.GUEST_IP, 40000, 7,
                                                b'asteria-unauthorized'))))
        add('peer', Lan.ethernet(Lan.GUEST_MAC, Lan.PEER_MAC, 0x800,
                               Lan.ipv4(Lan.PEER_IP, Lan.GUEST_IP, 17,
                                        Lan.udp(Lan.PEER_IP, Lan.GUEST_IP, 40000, 7, Lan.ECHO_PAYLOAD))))
        add('guest', guest_udp())
        self.lines = ['USER:' + line for line in Guest.NET_LINES]
        self.assertEqual(failures(Lan.judge_frames(self.frames, self.lines)), {})

    def test_missing_positive_evidence(self):
        for index, check in ((0, 'resolve'), (2, 'resolve'), (5, 'arp'), (8, 'icmp'), (11, 'udp-echo')):
            with self.subTest(index=index):
                failed = failures(Lan.judge_frames(self.frames[:index] + self.frames[index + 1:], self.lines))
                self.assertEqual(set(failed), {'host.amd64.net.' + check})

    def test_checks_are_named_for_the_isa(self):
        # IA-64's cases replay the same judgment under their own names.
        self.assertEqual(failures(Lan.judge_frames(self.frames, self.lines, isa='ia64')), {})
        failed = failures(Lan.judge_frames(self.frames[:11], self.lines, isa='ia64'))
        self.assertEqual(set(failed), {'host.ia64.net.udp-echo'})
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(set(failures(Lan.judge_artifacts(pathlib.Path(directory), self.lines, isa='ia64'))),
                             set(Lan.checks('ia64')))

    def test_missing_negative_controls(self):
        for index in (3, 6, 9):
            with self.subTest(index=index):
                failed = failures(Lan.judge_frames(self.frames[:index] + self.frames[index + 1:], self.lines))
                self.assertEqual(set(failed), {'host.amd64.net.negative'})

    def test_guest_source_and_checksums_on_unrelated_frames(self):
        # Append to the end: validation covers the whole capture even after
        # every positive reply and the guest's stopped line were recorded.
        source = bytearray(guest_udp(b'unrelated'))
        source[6] ^= 1
        ip_checksum = bytearray(guest_udp(b'unrelated'))
        ip_checksum[24] ^= 1
        icmp_checksum = bytearray(guest_echo(55, 66, b'unrelated'))
        icmp_checksum[36] ^= 1
        for data, detail in ((source, 'Ethernet source'), (ip_checksum, 'IPv4 header checksum'),
                             (icmp_checksum, 'ICMP checksum'), (b'truncated', 'truncated Ethernet')):
            with self.subTest(detail=detail):
                failed = failures(Lan.judge_frames(self.frames + [Lan.Frame('guest', 1700000010000000,
                                                                           bytes(data))], self.lines))
                self.assertEqual(set(failed), set(Lan.CHECKS))
                self.assertTrue(all(detail in d for d in failed.values()))

    def test_response_must_follow_request(self):
        for request, response, check in ((4, 5, 'arp'), (7, 8, 'icmp'), (10, 11, 'udp-echo')):
            frames = list(self.frames)
            frames[request], frames[response] = frames[response], frames[request]
            with self.subTest(check=check):
                failed = failures(Lan.judge_frames(frames, self.lines))
                self.assertEqual(set(failed), {'host.amd64.net.' + check})

    def test_exact_icmp_identity_payload_and_udp_addresses(self):
        original = list(self.frames)
        for data in (guest_echo(Lan.GOOD_ECHO[0] + 1, Lan.GOOD_ECHO[1], Lan.GOOD_ECHO[2]),
                     guest_echo(Lan.GOOD_ECHO[0], Lan.GOOD_ECHO[1] + 1, Lan.GOOD_ECHO[2]),
                     guest_echo(Lan.GOOD_ECHO[0], Lan.GOOD_ECHO[1], b'wrong')):
            frames = original[:8] + [dataclasses.replace(original[8], data=data)] + original[9:]
            self.assertEqual(set(failures(Lan.judge_frames(frames, self.lines))), {'host.amd64.net.icmp'})
        for data in (Lan.ethernet(Lan.GUEST_MAC, Lan.GUEST_MAC, 0x800,
                                 Lan.ipv4(Lan.GUEST_IP, Lan.PEER_IP, 17,
                                          Lan.udp(Lan.GUEST_IP, Lan.PEER_IP, 7, 40000, Lan.ECHO_PAYLOAD))),
                     Lan.ethernet(Lan.PEER_MAC, Lan.GUEST_MAC, 0x800,
                                 Lan.ipv4(Lan.GUEST_IP, Lan.PEER_IP, 17,
                                          Lan.udp(Lan.GUEST_IP, Lan.PEER_IP, 8, 40000, Lan.ECHO_PAYLOAD)))):
            frames = original[:11] + [dataclasses.replace(original[11], data=data)]
            self.assertEqual(set(failures(Lan.judge_frames(frames, self.lines))), {'host.amd64.net.udp-echo'})

    def test_missing_duplicate_or_bad_drop_line(self):
        for lines in ([], self.lines + ['USER:' + Lan.DROPS_LINE],
                      ['USER:# net: drops not-for-us 1, bad-icmp 1, unauthorized 0']):
            self.assertEqual(set(failures(Lan.judge_frames(self.frames, lines))), {'host.amd64.net.negative'})

    def test_recorded_peer_error_is_not_hidden_by_good_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            Lan.save_capture(path, self.frames, ['TimeoutError: LAN script timed out'])
            failed = failures(Lan.judge_artifacts(path, self.lines))
            self.assertEqual(set(failed), set(Lan.CHECKS))
            self.assertTrue(all('LAN script timed out' in detail for detail in failed.values()))


class FakeGuest:
    """Small socket responder: resolves before listening, drops controls, echoes.

    Mutants alter one behavior. The test joins its own thread and collects its
    exceptions; no daemon or external process is left behind after any test.
    """

    def __init__(self, connection, variant='normal', retry=False):
        self.connection = connection
        self.variant = variant
        self.retry = retry
        self.lines = ['USER:' + Guest.NET_LINES[0]]
        self.listening = threading.Event()
        self.echoed = threading.Event()
        self.cancel = threading.Event()
        self.errors = []
        self.thread = threading.Thread(target=self.run, name='fake-lan-guest')

    def run(self):
        counts = {'arp': 0, 'icmp': 0, 'udp': 0}
        self.connection.settimeout(0.05)
        try:
            self.connection.send(Lan.arp_request(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.PEER_IP))
            while not self.cancel.is_set():
                try:
                    data = self.connection.recv(65536)
                except socket.timeout:
                    continue
                f = Lan.decode_frame(data, verify=False)
                arp = f.get('arp', {})
                ip = f.get('ipv4', {})
                icmp, udp = ip.get('icmp', {}), ip.get('udp', {})
                if arp.get('opcode') == 2 and arp.get('sender_ip') == Lan.PEER_IP:
                    self.connection.send(guest_udp(Lan.RESOLVE_PAYLOAD))
                    self.lines.extend('USER:' + line for line in Guest.NET_LINES[1:3])
                    self.listening.set()
                elif arp.get('opcode') == 1:
                    target = arp['target_ip']
                    if target == Lan.OTHER_IP:
                        if self.variant == 'arp99':
                            self.connection.send(Lan.arp_reply(Lan.GUEST_MAC, target, Lan.PEER_MAC, Lan.PEER_IP))
                    elif target == Lan.GUEST_IP:
                        counts['arp'] += 1
                        if not self.retry or counts['arp'] > 1:
                            self.connection.send(Lan.arp_reply(Lan.GUEST_MAC, Lan.GUEST_IP,
                                                               Lan.PEER_MAC, Lan.PEER_IP))
                elif icmp:
                    if icmp['checksum_valid'] or self.variant == 'bad_echo':
                        counts['icmp'] += 1
                        if not self.retry or counts['icmp'] > 1:
                            self.connection.send(guest_echo(icmp['identifier'], icmp['sequence'],
                                                            bytes.fromhex(icmp['payload'])))
                elif udp:
                    if ip['source'] == Lan.UNAUTHORIZED_IP:
                        if self.variant == 'unauthorized_udp':
                            self.connection.send(guest_udp(bytes.fromhex(udp['payload']), Lan.UNAUTHORIZED_IP))
                        elif self.variant == 'unauthorized_icmp':
                            self.connection.send(guest_echo(17, 1, b'anything', Lan.UNAUTHORIZED_IP))
                    else:
                        counts['udp'] += 1
                        if self.retry and counts['udp'] == 1:
                            continue
                        payload = b'wrong-echo-bytes' if self.variant == 'wrong_bytes' else bytes.fromhex(udp['payload'])
                        self.connection.send(guest_udp(payload, bad_checksum=self.variant == 'bad_udp_checksum'))
                        if self.variant == 'send3':
                            self.connection.send(guest_udp(b'refused?', Lan.REFUSED_IP))
                        elif self.variant == 'arp3':
                            self.connection.send(Lan.arp_request(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.REFUSED_IP))
                        lines = list(Guest.NET_LINES[3:])
                        if self.variant == 'drop_counts':
                            lines[-2] = '# net: drops not-for-us 0, bad-icmp 2, unauthorized 1'
                        self.lines.extend('USER:' + line for line in lines)
                        self.echoed.set()
        except Exception as error:
            self.errors.append(error)
            self.listening.set()
            self.echoed.set()

    def stop(self):
        self.cancel.set()
        self.thread.join(2)
        if self.thread.is_alive():
            raise AssertionError('fake guest did not stop')


class PeerTests(unittest.TestCase):
    def demonstration(self, variant='normal', retry=False):
        with tempfile.TemporaryDirectory() as directory:
            ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
            peer = Lan.LanPeer(ours, pathlib.Path(directory)).start()
            guest = FakeGuest(theirs, variant, retry)
            guest.thread.start()
            try:
                self.assertTrue(guest.listening.wait(2), 'resolve before script did not complete')
                self.assertEqual(guest.errors, [])
                # ARP responder must have run before start_script was called.
                self.assertEqual(guest.lines, ['USER:' + line for line in Guest.NET_LINES[:3]])
                peer.start_script()
                peer.start_script()  # duplicate console polls must not duplicate controls
                self.assertTrue(guest.echoed.wait(10), 'script did not reach UDP echo')
                if variant != 'wrong_bytes':
                    self.assertTrue(peer.script_done.wait(2))
                # Wait for the last datagram(s), even after the script finished.
                self.assertIsNotNone(peer.wait_for(lambda f: f.direction == 'guest' and
                    f.decoded.get('ipv4', {}).get('udp', {}).get('payload') == (
                        b'wrong-echo-bytes' if variant == 'wrong_bytes' else Lan.ECHO_PAYLOAD).hex(), 2))
            finally:
                guest.stop()
                peer.stop()
                ours.close()
                theirs.close()
            self.assertEqual(guest.errors, [])
            self.assertEqual(peer.errors, [])
            self.assertFalse(peer._reader.is_alive())
            self.assertFalse(peer._script.is_alive())
            frames, errors = Lan.load_capture(pathlib.Path(directory))
            self.assertEqual(frames, peer.frames)
            self.assertEqual(errors, [])
            judged = Lan.judge_artifacts(pathlib.Path(directory), guest.lines)
            self.assertEqual(judged, Lan.judge_frames(frames, guest.lines))
            return failures(judged), frames, guest.lines

    def test_resolve_script_echo_lines_and_pacing(self):
        failed, frames, lines = self.demonstration()
        self.assertEqual(failed, {})
        self.assertEqual(lines, ['USER:' + line for line in Guest.NET_LINES])
        scripted = [f for f in frames if f.direction == 'peer' and f.decoded.get('arp', {}).get('opcode') != 2]
        self.assertEqual(len(scripted), 6)
        for first, second in zip(scripted, scripted[1:]):
            self.assertGreaterEqual(second.time_us - first.time_us, 500000)

    def test_retries_arp_icmp_udp(self):
        failed, frames, _ = self.demonstration(retry=True)
        self.assertEqual(failed, {})
        scripted = [f for f in frames if f.direction == 'peer' and f.decoded.get('arp', {}).get('opcode') != 2]
        self.assertEqual(len(scripted), 9)
        for first, second, minimum in ((scripted[1], scripted[2], 0.5),
                                       (scripted[4], scripted[5], 1), (scripted[7], scripted[8], 2)):
            self.assertGreaterEqual(second.time_us - first.time_us, minimum * 1000000 - 2000)

    def test_answers_other_arp(self):
        failed, _, _ = self.demonstration('arp99')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('ARP reply for 10.0.2.99', failed['host.amd64.net.negative'])

    def test_answers_bad_icmp(self):
        failed, _, _ = self.demonstration('bad_echo')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('reply to the bad ICMP', failed['host.amd64.net.negative'])

    def test_echoes_unauthorized_udp(self):
        failed, _, _ = self.demonstration('unauthorized_udp')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('unauthorized 10.0.2.4', failed['host.amd64.net.negative'])

    def test_answers_unauthorized_with_another_protocol(self):
        failed, _, _ = self.demonstration('unauthorized_icmp')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('unauthorized 10.0.2.4', failed['host.amd64.net.negative'])

    def test_wrong_echo_bytes(self):
        failed, _, _ = self.demonstration('wrong_bytes')
        self.assertEqual(set(failed), {'host.amd64.net.udp-echo'})
        self.assertIn('exactly', failed['host.amd64.net.udp-echo'])

    def test_wrong_udp_checksum(self):
        failed, _, _ = self.demonstration('bad_udp_checksum')
        self.assertEqual(set(failed), set(Lan.CHECKS))
        self.assertTrue(all('bad UDP checksum' in detail for detail in failed.values()))

    def test_sends_ipv4_to_refused_address(self):
        failed, _, _ = self.demonstration('send3')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('refused 10.0.2.3', failed['host.amd64.net.negative'])

    def test_sends_arp_to_refused_address(self):
        failed, _, _ = self.demonstration('arp3')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('refused 10.0.2.3', failed['host.amd64.net.negative'])

    def test_wrong_drop_counts(self):
        failed, _, _ = self.demonstration('drop_counts')
        self.assertEqual(set(failed), {'host.amd64.net.negative'})
        self.assertIn('expected exactly', failed['host.amd64.net.negative'])

    def test_bounded_wait_since_and_stop_without_closing_socket(self):
        with tempfile.TemporaryDirectory() as directory:
            ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
            self.addCleanup(ours.close)
            self.addCleanup(theirs.close)
            peer = Lan.LanPeer(ours, pathlib.Path(directory)).start()
            self.addCleanup(peer.stop)
            frame = Lan.arp_request(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.PEER_IP)
            theirs.send(frame)
            self.assertIsNotNone(peer.wait_for(lambda f: f.direction == 'guest', 1))
            index = peer.index
            self.assertIsNone(peer.wait_for(lambda f: f.direction == 'guest', 0.05, since=index))
            # Retry helper has a finite deadline when a responder is absent.
            with self.assertRaises(TimeoutError):
                peer._retry(frame, lambda f: False, 0.02, 0.06)
            peer.start_script()
            started = time.monotonic()
            peer.stop()
            self.assertLess(time.monotonic() - started, 1)
            peer.stop()  # idempotent
            self.assertIsNone(ours.gettimeout())
            ours.send(b'still open')
            self.assertEqual(peer.errors, [])


if __name__ == '__main__':
    unittest.main()
