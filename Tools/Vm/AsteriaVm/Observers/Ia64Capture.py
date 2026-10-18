# SPDX-License-Identifier: Apache-2.0
"""One IA-64 case's observation, and its files in the run directory.

A driver waits for the guest, stops it, and captures an `Observation`: the
debug-channel lines, each requested CPU's register dump, named memory dumps, and QEMU's
own CPU and PCI view. Evaluating a case is then a pure function of the
observation and the expectation, so the same run-directory files serve as
the failure artifacts, as input for evaluating a run again, and as the unit
tests' recorded fixtures.

Files: `Debug.log` (the channel as the console received it), `Registers.txt`
(`info registers` of CPU 0 after the stop), optional `Registers.Cpu<N>.txt`
for AP captures, `<Name>.bin` per memory dump,
`Pci.json` (`query-pci`), `Cpus.json` (`query-cpus-fast`), `LoopWait.json`
(the loop wait's outcome) and, where the case has a NIC, the LAN peer's
capture (`Lan.pcap` and `Lan.json`, which `AsteriaVm.Lan` writes).
"""
from __future__ import annotations

import dataclasses
import json
import pathlib

from AsteriaVm import Lan
from AsteriaVm.Observers.Ia64Cpu import LoopWait

DEBUG_LOG = 'Debug.log'
REGISTERS = 'Registers.txt'
PCI = 'Pci.json'
CPUS = 'Cpus.json'
LOOP_WAIT = 'LoopWait.json'


@dataclasses.dataclass
class Observation:
    lines: list[str]
    registers: str | None = None
    memory: dict[str, bytes] = dataclasses.field(default_factory=dict)
    pci: list | None = None
    cpus: list | None = None
    loop: LoopWait | None = None
    processor_registers: dict[int, str] = dataclasses.field(default_factory=dict)
    # The LAN peer's frames and recorded errors (Lan.load_capture), None
    # without a capture; a capture that cannot be replayed is no frames and
    # its error.
    lan: tuple[list[Lan.Frame], list[str]] | None = None


def load_lan(directory: pathlib.Path) -> tuple[list[Lan.Frame], list[str]] | None:
    """The LAN peer's capture in a run directory, if it wrote one."""
    if not any((directory / name).exists() for name in (Lan.PCAP_ARTIFACT, Lan.JSON_ARTIFACT)):
        return None
    try:
        return Lan.load_capture(directory)
    except (OSError, ValueError) as error:
        return [], ['cannot replay LAN artifacts: %s' % error]


def lines_from_log(data: bytes) -> list[str]:
    """The lines `Console` framed from these bytes (LF-split, one trailing CR
    removed, NUL kept), the unterminated tail included as on close."""
    lines = []
    for raw in data.split(b'\n'):
        if raw.endswith(b'\r'):
            raw = raw[:-1]
        lines.append(raw.decode('ascii', errors='replace'))
    if lines and lines[-1] == '':
        lines.pop()
    return lines


def save(observation: Observation, directory: pathlib.Path) -> None:
    """Writes everything but Debug.log, which the console already tees."""
    if observation.registers is not None:
        (directory / REGISTERS).write_text(observation.registers)
    for processor, registers in observation.processor_registers.items():
        (directory / ('Registers.Cpu%d.txt' % processor)).write_text(registers)
    for name, data in observation.memory.items():
        (directory / ('%s.bin' % name)).write_bytes(data)
    if observation.pci is not None:
        (directory / PCI).write_text(json.dumps(observation.pci, indent=1) + '\n')
    if observation.cpus is not None:
        (directory / CPUS).write_text(json.dumps(observation.cpus, indent=1) + '\n')
    if observation.loop is not None:
        (directory / LOOP_WAIT).write_text(json.dumps(dataclasses.asdict(observation.loop), indent=1) + '\n')


def load(directory: pathlib.Path) -> Observation:
    """The observation a run directory (or a recorded fixture) holds."""
    def optional_json(name: str):
        path = directory / name
        return json.loads(path.read_text()) if path.exists() else None

    registers = directory / REGISTERS
    loop = optional_json(LOOP_WAIT)
    if loop is not None:
        loop = LoopWait(loop['reached'], loop['polls'], loop['matches'],
                        tuple(loop['last']) if loop['last'] is not None else None, loop['reason'])
    return Observation(lines_from_log((directory / DEBUG_LOG).read_bytes()),
                       registers.read_text() if registers.exists() else None,
                       {path.stem: path.read_bytes() for path in sorted(directory.glob('*.bin'))},
                       optional_json(PCI), optional_json(CPUS), loop,
                       {int(path.stem.removeprefix('Registers.Cpu')): path.read_text()
                        for path in sorted(directory.glob('Registers.Cpu*.txt'))}, load_lan(directory))
