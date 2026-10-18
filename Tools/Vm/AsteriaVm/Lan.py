# SPDX-License-Identifier: Apache-2.0
"""Independent Ethernet LAN peer and replay checks for network service contract.

Wire formats: RFC 826 (ARP), 791 (IPv4 without options or fragmentation),
792 (ICMP echo), 768 (UDP), 1071 (Internet checksum). No guest implementation
or source constants are used. Each socket datagram is one frame without FCS.
Lan.pcap is classic microsecond libpcap, LINKTYPE_ETHERNET; Lan.json records
its direction, timestamp and independently decoded summary in capture order.
"""
from __future__ import annotations

import dataclasses
import ipaddress
import json
import pathlib
import socket
import struct
import threading
import time
from collections.abc import Callable

GUEST_MAC = '52:54:00:a5:7e:01'
PEER_MAC = '52:54:00:a5:7e:02'
BROADCAST = 'ff:ff:ff:ff:ff:ff'
ZERO_MAC = '00:00:00:00:00:00'
GUEST_IP = '10.0.2.15'
PEER_IP = '10.0.2.2'
OTHER_IP = '10.0.2.99'
UNAUTHORIZED_IP = '10.0.2.4'
REFUSED_IP = '10.0.2.3'
RESOLVE_PAYLOAD = b'asteria-resolve'
ECHO_PAYLOAD = b'asteria-udp-echo'
BAD_ECHO = (0xA501, 1, b'asteria-bad-icmp')
GOOD_ECHO = (0xA502, 2, b'asteria-good-icmp')
LISTENING_LINE = '# net: listening on udp 7'
DROPS_LINE = '# net: drops not-for-us 1, bad-icmp 1, unauthorized 1'
CHECK_NAMES = ('resolve', 'arp', 'icmp', 'udp-echo', 'negative')


def checks(isa: str = 'amd64') -> tuple[str,...]:
    """The LAN check names of an ISA's cases (`host.<isa>.net.*`): the same
    ISA-neutral judgment on either."""
    return tuple('host.%s.net.%s' % (isa, name) for name in CHECK_NAMES)


CHECKS = checks()
# The least frames the network demonstration's NIC driver copies through its
# client page: the peer's ARP request for another address, its ARP request
# and echo requests, the bad echo, the two datagrams and its ARP reply to the
# stack's request (7), and the stack's ARP request, the resolving datagram,
# its ARP reply, echo reply and the datagram's echo (5).
LEAST_COPIES = 12
PCAP_ARTIFACT = 'Lan.pcap'
JSON_ARTIFACT = 'Lan.json'


def checksum(data: bytes) -> int:
    """RFC 1071 one's-complement sum, with a zero byte for odd lengths."""
    data += b'\0' if len(data) & 1 else b''
    total = sum(int.from_bytes(data[i:i + 2], 'big') for i in range(0, len(data), 2))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def _mac(address: str) -> bytes:
    data = bytes.fromhex(address.replace(':', ''))
    if len(data) != 6:
        raise ValueError('MAC must have six bytes')
    return data


def _mac_text(data: bytes) -> str:
    return ':'.join('%02x' % byte for byte in data)


def _ip(address: str) -> bytes:
    return ipaddress.IPv4Address(address).packed


def _ip_text(data: bytes) -> str:
    return str(ipaddress.IPv4Address(data))


def pad_frame(frame: bytes) -> bytes:
    return frame + bytes(max(0, 60 - len(frame)))


def ethernet(destination: str, source: str, ethertype: int, payload: bytes) -> bytes:
    if ethertype < 0x600:
        raise ValueError('Ethernet II needs an EtherType, not an IEEE 802.3 length')
    return pad_frame(_mac(destination) + _mac(source) + struct.pack('!H', ethertype) + payload)


def parse_ethernet(data: bytes) -> dict:
    if len(data) < 14:
        raise ValueError('truncated Ethernet header')
    kind = int.from_bytes(data[12:14], 'big')
    if kind < 0x600:
        raise ValueError('not Ethernet II')
    return dict(destination=_mac_text(data[:6]), source=_mac_text(data[6:12]), ethertype=kind,
                payload=data[14:].hex())


def arp(opcode: int, sender_mac: str, sender_ip: str, target_mac: str, target_ip: str) -> bytes:
    if opcode not in (1, 2):
        raise ValueError('ARP must be request or reply')
    return (struct.pack('!HHBBH', 1, 0x800, 6, 4, opcode) + _mac(sender_mac) + _ip(sender_ip)
            + _mac(target_mac) + _ip(target_ip))


def arp_request(sender_mac: str, sender_ip: str, target_ip: str) -> bytes:
    return ethernet(BROADCAST, sender_mac, 0x806, arp(1, sender_mac, sender_ip, ZERO_MAC, target_ip))


def arp_reply(sender_mac: str, sender_ip: str, target_mac: str, target_ip: str) -> bytes:
    return ethernet(target_mac, sender_mac, 0x806, arp(2, sender_mac, sender_ip, target_mac, target_ip))


def parse_arp(data: bytes) -> dict:
    if len(data) < 28:
        raise ValueError('truncated ARP packet')
    hardware, protocol, hlen, plen, opcode = struct.unpack_from('!HHBBH', data)
    if (hardware, protocol, hlen, plen) != (1, 0x800, 6, 4) or opcode not in (1, 2):
        raise ValueError('unsupported ARP format or opcode')
    return dict(opcode=opcode, sender_mac=_mac_text(data[8:14]), sender_ip=_ip_text(data[14:18]),
                target_mac=_mac_text(data[18:24]), target_ip=_ip_text(data[24:28]))


def ipv4(source: str, destination: str, protocol: int, payload: bytes, *, identification: int = 0,
         ttl: int = 64, tos: int = 0, dont_fragment: bool = False) -> bytes:
    header = struct.pack('!BBHHHBBH4s4s', 0x45, tos, 20 + len(payload), identification,
                         0x4000 if dont_fragment else 0, ttl, protocol, 0, _ip(source), _ip(destination))
    return header[:10] + struct.pack('!H', checksum(header)) + header[12:] + payload


def parse_ipv4(data: bytes, *, verify: bool = True) -> dict:
    if len(data) < 20:
        raise ValueError('truncated IPv4 header')
    version, tos, length, ident, flags, ttl, protocol, check, source, destination = struct.unpack_from(
        '!BBHHHBBH4s4s', data)
    if version != 0x45:
        raise ValueError('IPv4 requires version 4 and IHL 5')
    if length < 20 or length > len(data):
        raise ValueError('invalid or truncated IPv4 total length')
    if flags & ~0x4000:
        raise ValueError('fragmented IPv4 or reserved flag')
    valid = checksum(data[:20]) == 0
    if verify and not valid:
        raise ValueError('bad IPv4 header checksum')
    return dict(source=_ip_text(source), destination=_ip_text(destination), protocol=protocol,
                identification=ident, ttl=ttl, tos=tos, dont_fragment=bool(flags & 0x4000),
                checksum=check, checksum_valid=valid, payload=data[20:length].hex())


def icmp_echo(identifier: int, sequence: int, payload: bytes, *, reply: bool = False) -> bytes:
    packet = struct.pack('!BBHHH', 0 if reply else 8, 0, 0, identifier, sequence) + payload
    return packet[:2] + struct.pack('!H', checksum(packet)) + packet[4:]


def parse_icmp(data: bytes, *, verify: bool = True) -> dict:
    if len(data) < 8:
        raise ValueError('truncated ICMP echo')
    kind, code, check, identifier, sequence = struct.unpack_from('!BBHHH', data)
    if kind not in (0, 8) or code:
        raise ValueError('unsupported ICMP type or code')
    valid = checksum(data) == 0
    if verify and not valid:
        raise ValueError('bad ICMP checksum')
    return dict(type=kind, identifier=identifier, sequence=sequence, checksum=check,
                checksum_valid=valid, payload=data[8:].hex())


def _pseudo_header(source: str, destination: str, length: int) -> bytes:
    return _ip(source) + _ip(destination) + struct.pack('!BBH', 0, 17, length)


def udp(source: str, destination: str, source_port: int, destination_port: int, payload: bytes,
        *, with_checksum: bool = True) -> bytes:
    length = 8 + len(payload)
    packet = struct.pack('!HHHH', source_port, destination_port, length, 0) + payload
    check = (checksum(_pseudo_header(source, destination, length) + packet) or 0xFFFF) if with_checksum else 0
    return packet[:6] + struct.pack('!H', check) + packet[8:]


def parse_udp(data: bytes, source: str, destination: str, *, verify: bool = True) -> dict:
    if len(data) < 8:
        raise ValueError('truncated UDP header')
    sport, dport, length, check = struct.unpack_from('!HHHH', data)
    if length < 8 or length != len(data):
        raise ValueError('invalid UDP length')
    valid = check == 0 or checksum(_pseudo_header(source, destination, length) + data) == 0
    if verify and not valid:
        raise ValueError('bad UDP checksum')
    return dict(source_port=sport, destination_port=dport, checksum=check,
                checksum_valid=valid, payload=data[8:].hex())


def decode_frame(data: bytes, *, verify: bool = True) -> dict:
    frame = parse_ethernet(data)
    payload = bytes.fromhex(frame.pop('payload'))
    if frame['ethertype'] == 0x806:
        frame['arp'] = parse_arp(payload)
    elif frame['ethertype'] == 0x800:
        packet = parse_ipv4(payload, verify=verify)
        transport = bytes.fromhex(packet.pop('payload'))
        if packet['protocol'] == 1:
            packet['icmp'] = parse_icmp(transport, verify=verify)
        elif packet['protocol'] == 17:
            packet['udp'] = parse_udp(transport, packet['source'], packet['destination'], verify=verify)
        else:
            packet['payload'] = transport.hex()
        frame['ipv4'] = packet
    else:
        frame['payload'] = payload.hex()
    return frame


def summary(data: bytes) -> dict:
    try:
        return decode_frame(data, verify=False)
    except ValueError as error:
        return {'error': str(error)}


@dataclasses.dataclass(frozen=True)
class Frame:
    direction: str  # 'guest' = from guest; 'peer' = to guest
    time_us: int    # Unix epoch, microseconds (same value in both artifacts)
    data: bytes

    @property
    def decoded(self) -> dict:
        return summary(self.data)


def write_pcap(path: pathlib.Path, frames: list[Frame]) -> None:
    with path.open('wb') as handle:
        handle.write(struct.pack('<IHHIIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for frame in frames:
            sec, usec = divmod(frame.time_us, 1_000_000)
            handle.write(struct.pack('<IIII', sec, usec, len(frame.data), len(frame.data)))
            handle.write(frame.data)


def read_pcap(path: pathlib.Path) -> list[tuple[int, bytes]]:
    data = path.read_bytes()
    if len(data) < 24 or data[:4] not in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4'):
        raise ValueError('invalid classic microsecond pcap header')
    endian = '<' if data[:4] == b'\xd4\xc3\xb2\xa1' else '>'
    major, minor, zone, sigfigs, snaplen, linktype = struct.unpack_from(endian + 'HHIIII', data, 4)
    if (major, minor, linktype) != (2, 4, 1) or not 14 <= snaplen <= 65535:
        raise ValueError('unsupported pcap version, snaplen or link type')
    frames, offset = [], 24
    while offset < len(data):
        if offset + 16 > len(data):
            raise ValueError('truncated pcap record header')
        sec, usec, captured, original = struct.unpack_from(endian + 'IIII', data, offset)
        offset += 16
        if usec >= 1_000_000 or captured != original or captured > snaplen or offset + captured > len(data):
            raise ValueError('truncated or invalid pcap frame')
        frames.append((sec * 1_000_000 + usec, data[offset:offset + captured]))
        offset += captured
    return frames


def save_capture(directory: pathlib.Path, frames: list[Frame], errors: list[str] | None = None) -> None:
    write_pcap(directory / PCAP_ARTIFACT, frames)
    record = dict(version=1, errors=list(errors or []), frames=[
        dict(direction=f.direction, time_us=f.time_us, summary=f.decoded) for f in frames])
    (directory / JSON_ARTIFACT).write_text(json.dumps(record, indent=2) + '\n')


def load_capture(directory: pathlib.Path) -> tuple[list[Frame], list[str]]:
    """Read bytes from pcap, direction from JSON; verify the two records agree."""
    missing = [name for name in (PCAP_ARTIFACT, JSON_ARTIFACT) if not (directory / name).is_file()]
    if missing:
        raise ValueError('missing LAN artifact: ' + ', '.join(missing))
    packets = read_pcap(directory / PCAP_ARTIFACT)
    record = json.loads((directory / JSON_ARTIFACT).read_text())
    if not isinstance(record, dict) or record.get('version') != 1 or not isinstance(record.get('frames'), list):
        raise ValueError('invalid Lan.json format')
    rows, errors = record['frames'], record.get('errors')
    if not isinstance(errors, list) or any(not isinstance(e, str) for e in errors) or len(rows) != len(packets):
        raise ValueError('invalid Lan.json errors or pcap/JSON frame count mismatch')
    frames = []
    for index, (row, (timestamp, data)) in enumerate(zip(rows, packets)):
        if (not isinstance(row, dict) or row.get('direction') not in ('guest', 'peer')
                or row.get('time_us') != timestamp or row.get('summary') != summary(data)):
            raise ValueError('pcap/JSON mismatch at frame %d' % index)
        frames.append(Frame(row['direction'], timestamp, data))
    return frames, errors


def _arp_is(frame: dict, opcode: int, sender_ip: str, sender_mac: str, target_ip: str,
            target_mac: str, destination: str) -> bool:
    return (frame.get('destination') == destination and frame.get('source') == sender_mac
            and frame.get('arp') == dict(opcode=opcode, sender_ip=sender_ip, sender_mac=sender_mac,
                                         target_ip=target_ip, target_mac=target_mac))


def _udp_is(frame: dict, source: str, destination: str, sport: int, dport: int,
            payload: bytes, source_mac: str, destination_mac: str) -> bool:
    packet = frame.get('ipv4', {})
    datagram = packet.get('udp', {})
    return (frame.get('source') == source_mac and frame.get('destination') == destination_mac
            and packet.get('source') == source and packet.get('destination') == destination
            and datagram.get('source_port') == sport and datagram.get('destination_port') == dport
            and datagram.get('payload') == payload.hex())


def _echo_is(frame: dict, echo: tuple[int, int, bytes], *, reply: bool) -> bool:
    packet = frame.get('ipv4', {})
    message = packet.get('icmp', {})
    src, dst, smac, dmac = ((GUEST_IP, PEER_IP, GUEST_MAC, PEER_MAC) if reply else
                            (PEER_IP, GUEST_IP, PEER_MAC, GUEST_MAC))
    return (frame.get('source') == smac and frame.get('destination') == dmac
            and packet.get('source') == src and packet.get('destination') == dst
            and message.get('type') == (0 if reply else 8) and message.get('identifier') == echo[0]
            and message.get('sequence') == echo[1] and message.get('payload') == echo[2].hex())


class LanPeer:
    """Reader/ARP responder from case start; a separate, cancellable script thread.

    The executor owns the socket. stop joins our threads, drains queued guest
    frames and saves artifacts, including on a failed boot. It never closes it.
    """

    def __init__(self, connection: socket.socket, directory: pathlib.Path):
        self.connection = connection
        self.directory = pathlib.Path(directory)
        self.frames: list[Frame] = []
        self.errors: list[str] = []
        self._condition = threading.Condition()
        self._cancel = threading.Event()
        self._stop_reader = threading.Event()
        self._drain_deadline = float('inf')
        self._reader: threading.Thread | None = None
        self._script: threading.Thread | None = None
        self._timeout = connection.gettimeout()
        self._stopped = False
        self.script_done = threading.Event()

    def start(self) -> 'LanPeer':
        if self._reader is None:
            self.connection.settimeout(0.05)
            self._reader = threading.Thread(target=self._read, name='lan-reader', daemon=True)
            self._reader.start()
        return self

    def _record(self, direction: str, data: bytes) -> None:
        # Called under the condition: send/record cannot be reordered with receive/record.
        self.frames.append(Frame(direction, time.time_ns() // 1000, data))
        self._condition.notify_all()

    def _error(self, error: Exception) -> None:
        with self._condition:
            self.errors.append('%s: %s' % (type(error).__name__, error))
            self._condition.notify_all()

    def _read(self) -> None:
        try:
            while True:
                if self._stop_reader.is_set() and time.monotonic() >= self._drain_deadline:
                    raise TimeoutError('LAN guest kept sending during stop')
                try:
                    data = self.connection.recv(65536)
                except socket.timeout:
                    if self._stop_reader.is_set():
                        break
                    continue
                with self._condition:
                    self._record('guest', data)
                frame = summary(data)
                request = frame.get('arp', {})
                if request.get('opcode') == 1 and request.get('target_ip') == PEER_IP:
                    self.send(arp_reply(PEER_MAC, PEER_IP, request['sender_mac'], request['sender_ip']))
        except OSError as error:
            self._error(error)

    @property
    def index(self) -> int:
        with self._condition:
            return len(self.frames)

    def send(self, data: bytes) -> None:
        data = pad_frame(data)
        with self._condition:
            if self.connection.send(data) != len(data):
                raise OSError('short LAN datagram send')
            self._record('peer', data)

    def wait_for(self, predicate: Callable[[Frame], bool], timeout: float, *, since: int = 0) -> Frame | None:
        deadline = time.monotonic() + timeout
        with self._condition:
            cursor = since
            while True:
                for frame in self.frames[cursor:]:
                    if predicate(frame):
                        return frame
                cursor = len(self.frames)
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self._cancel.is_set() or self.errors:
                    return None
                self._condition.wait(remaining)

    def start_script(self) -> None:
        if self._script is None:
            self._script = threading.Thread(target=self._play, name='lan-script', daemon=True)
            self._script.start()

    def _retry(self, data: bytes, predicate: Callable[[dict], bool], interval: float, timeout: float) -> None:
        deadline, since = time.monotonic() + timeout, self.index
        while not self._cancel.is_set():
            if self.errors:
                raise OSError('LAN reader failed')
            self.send(data)
            if self.wait_for(lambda f: f.direction == 'guest' and predicate(f.decoded),
                             min(interval, max(0, deadline - time.monotonic())), since=since):
                return
            if time.monotonic() >= deadline:
                raise TimeoutError('LAN script timed out after %g s' % timeout)
        raise InterruptedError('LAN script stopped')

    def _pause(self) -> None:
        if self._cancel.wait(0.5):
            raise InterruptedError('LAN script stopped')

    def _play(self) -> None:
        try:
            self.send(arp_request(PEER_MAC, PEER_IP, OTHER_IP))
            self._pause()
            self._retry(arp_request(PEER_MAC, PEER_IP, GUEST_IP),
                        lambda f: _arp_is(f, 2, GUEST_IP, GUEST_MAC, PEER_IP, PEER_MAC, PEER_MAC), 0.5, 30)
            self._pause()
            bad = bytearray(icmp_echo(*BAD_ECHO))
            bad[2] ^= 1
            self.send(ethernet(GUEST_MAC, PEER_MAC, 0x800, ipv4(PEER_IP, GUEST_IP, 1, bytes(bad))))
            self._pause()
            self._retry(ethernet(GUEST_MAC, PEER_MAC, 0x800, ipv4(PEER_IP, GUEST_IP, 1,
                        icmp_echo(*GOOD_ECHO))), lambda f: _echo_is(f, GOOD_ECHO, reply=True), 1, 30)
            self._pause()
            self.send(ethernet(GUEST_MAC, PEER_MAC, 0x800, ipv4(UNAUTHORIZED_IP, GUEST_IP, 17,
                      udp(UNAUTHORIZED_IP, GUEST_IP, 40000, 7, b'asteria-unauthorized'))))
            self._pause()
            self._retry(ethernet(GUEST_MAC, PEER_MAC, 0x800, ipv4(PEER_IP, GUEST_IP, 17,
                        udp(PEER_IP, GUEST_IP, 40000, 7, ECHO_PAYLOAD))),
                        lambda f: _udp_is(f, GUEST_IP, PEER_IP, 7, 40000, ECHO_PAYLOAD, GUEST_MAC, PEER_MAC), 2, 60)
        except InterruptedError:
            pass  # Stopping a failed/incomplete run is normal; missing evidence fails replay
        except Exception as error:
            self._error(error)
        finally:
            self.script_done.set()

    def stop(self) -> None:
        if self._stopped:
            return
        self._cancel.set()
        with self._condition:
            self._condition.notify_all()
        if self._script is not None:
            self._script.join()
        self._drain_deadline = time.monotonic() + 0.5
        self._stop_reader.set()
        if self._reader is not None:
            self._reader.join()
        self.connection.settimeout(self._timeout)
        save_capture(self.directory, self.frames, self.errors)
        self._stopped = True


def judge_frames(frames: list[Frame], lines: list[str], errors: list[str] | None = None,
                 isa: str = 'amd64') -> dict[str, list[str]]:
    """Pure replay judge. Positive evidence must follow its matching stimulus.

    Every guest frame is validated, including frames unrelated to the probes;
    checksum/source failures fail all five LAN checks. Negative controls must
    appear in the capture, so an empty or incomplete capture cannot pass them.
    The checks are named for `isa` (`checks`).
    """
    names = checks(isa)
    problems = {name: list(errors or []) for name in names}
    decoded = [f.decoded for f in frames]
    guests = [(i, d) for i, (f, d) in enumerate(zip(frames, decoded)) if f.direction == 'guest']
    peers = [(i, d) for i, (f, d) in enumerate(zip(frames, decoded)) if f.direction == 'peer']
    invalid = []
    for index, frame in guests:
        if frame.get('source') != GUEST_MAC:
            invalid.append('guest frame %d: Ethernet source is not %s' % (index, GUEST_MAC))
        try:
            decode_frame(frames[index].data)
        except ValueError as error:
            invalid.append('guest frame %d: %s' % (index, error))
    for value in problems.values():
        value.extend(invalid)

    def need(name: str, passed: bool, detail: str) -> None:
        if not passed:
            problems['host.%s.net.%s' % (isa, name)].append(detail)

    def follows(stimulus, response) -> bool:
        return any(response(g) and gi > pi for pi, p in peers if stimulus(p) for gi, g in guests)

    resolving = [(i, f) for i, f in guests if _arp_is(
        f, 1, GUEST_IP, GUEST_MAC, PEER_IP, ZERO_MAC, BROADCAST)]
    need('resolve', bool(resolving), 'no guest broadcast ARP request resolving 10.0.2.2')
    need('resolve', any(_udp_is(g, GUEST_IP, PEER_IP, 7, 40000, RESOLVE_PAYLOAD, GUEST_MAC, PEER_MAC)
                           and gi > ai for ai, _ in resolving for gi, g in guests),
         'no asteria-resolve datagram after the resolving ARP request')
    need('arp', follows(lambda f: _arp_is(f, 1, PEER_IP, PEER_MAC, GUEST_IP, ZERO_MAC, BROADCAST),
                        lambda f: _arp_is(f, 2, GUEST_IP, GUEST_MAC, PEER_IP, PEER_MAC, PEER_MAC)),
         'no ARP reply naming the NIC MAC to the peer request')
    need('icmp', follows(lambda f: _echo_is(f, GOOD_ECHO, reply=False)
                        and f['ipv4']['icmp']['checksum_valid'],
                        lambda f: _echo_is(f, GOOD_ECHO, reply=True)), 'no mirrored good ICMP echo reply to the peer')
    need('udp-echo', follows(lambda f: _udp_is(f, PEER_IP, GUEST_IP, 40000, 7, ECHO_PAYLOAD, PEER_MAC, GUEST_MAC),
                            lambda f: _udp_is(f, GUEST_IP, PEER_IP, 7, 40000, ECHO_PAYLOAD, GUEST_MAC, PEER_MAC)),
         'no UDP echo to the peer with exactly b"asteria-udp-echo"')
    need('negative', sum(_arp_is(f, 1, PEER_IP, PEER_MAC, OTHER_IP, ZERO_MAC, BROADCAST)
                         for _, f in peers) == 1, 'expected one peer ARP request for 10.0.2.99')
    need('negative', sum(_echo_is(f, BAD_ECHO, reply=False) and not f['ipv4']['icmp']['checksum_valid']
                         for _, f in peers) == 1, 'expected one peer ICMP echo request with bad checksum')
    need('negative', sum(f.get('ipv4', {}).get('source') == UNAUTHORIZED_IP
                         and _udp_is(f, UNAUTHORIZED_IP, GUEST_IP, 40000, 7, b'asteria-unauthorized',
                                     PEER_MAC, GUEST_MAC) for _, f in peers) == 1,
         'expected one unauthorized peer UDP datagram from 10.0.2.4')
    for index, frame in guests:
        a, p = frame.get('arp', {}), frame.get('ipv4', {})
        need('negative', not (a.get('opcode') == 2 and a.get('sender_ip') == OTHER_IP),
             'guest frame %d: ARP reply for 10.0.2.99' % index)
        message = p.get('icmp', {})
        need('negative', not (message.get('type') == 0 and message.get('identifier') == BAD_ECHO[0]
                             and message.get('sequence') == BAD_ECHO[1]),
             'guest frame %d: reply to the bad ICMP echo' % index)
        need('negative', p.get('destination') != UNAUTHORIZED_IP and a.get('target_ip') != UNAUTHORIZED_IP,
             'guest frame %d: sent to unauthorized 10.0.2.4' % index)
        need('negative', p.get('destination') != REFUSED_IP and a.get('target_ip') != REFUSED_IP,
             'guest frame %d: sent toward refused 10.0.2.3' % index)
    channel = [line.removeprefix('USER:') for line in lines]
    drops = [line for line in channel if line.startswith('# net: drops')]
    need('negative', drops == [DROPS_LINE], 'expected exactly %r; found %r' % (DROPS_LINE, drops))
    return problems


def judge_artifacts(directory: pathlib.Path, lines: list[str], isa: str = 'amd64') -> dict[str, list[str]]:
    try:
        frames, errors = load_capture(pathlib.Path(directory))
    except (OSError, ValueError) as error:
        return {name: ['cannot replay LAN artifacts: %s' % error] for name in checks(isa)}
    return judge_frames(frames, lines, errors, isa)
