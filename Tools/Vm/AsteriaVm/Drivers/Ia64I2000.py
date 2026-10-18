# SPDX-License-Identifier: Apache-2.0
"""QEMU's HP i2000 boots the production firmware to the root service.

The machine (Merced, 460GX) has no ACPI debug port: its SPCR and HCDP name
COM1, the firmware console, which is the diagnostic channel (Machine.prepare,
BootProtocol.md "Debug port"). The emulator's -debug-port is attached but
publishes no DBGP there, so nothing may reach it. It has no ECAM, so the
kernel binds no PCI function, reads no AHCI disk and publishes no scanout;
the FADT publishes its i8042. The case checks the boot path's stage events (each once and in order,
none of the ECAM path), the idle loop at PL3, the root service's readiness
after the boot inventory, and typed input and (on one CPU) key repeat through
the resident PS/2 terminal.
"""
from __future__ import annotations

from AsteriaVerify.Elf import ElfFile
from AsteriaVm import Guest, Ia64Layout, Ps2, Stages
from AsteriaVm.Drivers import Ia64Default, Production

ECAM_STAGES = ('PCI-ECAM', 'PCI-DISCOVERY')
READY = '# init: ready: display, input, ps2'


def run(context) -> None:
    idle = ElfFile(context.image('user_elf')).symbol(Ia64Layout.USER_IDLE_SYMBOL).value
    psr = Ia64Layout.headers(context.target.root).user_psr
    Ia64Default.gate(context, Ia64Default.LAUNCH_STAGE, Ia64Default.BOOT_TIMEOUT)
    loop = Ia64Default.wait_for_user_loop(context, idle, psr)
    context.check(Production.CHECK, loop.reached, loop.describe(idle, psr))
    context.wait_for_line('USER:' + READY, 60.0)
    channel = Guest.channel_lines(context.console.lines)
    inventory = next((index for index, line in enumerate(channel)
                      if line.startswith('# devices: input 8042=present')), None)
    context.check('product.init-ready', READY in channel, 'the root service started display, input and PS/2')
    context.check('product.inventory', inventory is not None and inventory < channel.index(READY),
                  'boot inventory, with the FADT i8042, precedes init readiness')
    Ps2.inject_terminal(context)
    context.wait_for_line('USER:asteria', 10.0)
    context.check('product.terminal-serial', 'asteria' in Guest.channel_lines(context.console.lines),
                  'typed asteria through the PS/2 terminal')
    if context.case.cpus == 1:
        Production.repeat_terminal(context)
    else:
        # Held-key repeat is unsupported on multi-CPU IA-64 boots.
        context.check('terminal.repeat', True, skipped='held keys do not repeat with more than one IA-64 CPU')
    expected = tuple(stage for stage in Ia64Default.BOOT_STAGES if stage not in ECAM_STAGES)
    expected += Ia64Default.DEFAULT_STAGES
    absent = ECAM_STAGES + Ia64Default.DISK_STAGES + Ia64Default.PROBE_STAGES[:1] + (Ia64Default.PREEMPTION_STAGE,)
    context.check_problems('host.stages', Stages.problems(context.console.lines, expected, absent=absent))
    unattached = context.artifact('DebugPort.log')
    context.check('host.i2000.debug-port', unattached.exists() and unattached.stat().st_size == 0,
                  'hp-i2000 publishes no DBGP for -debug-port: every diagnostic came on COM1, which its SPCR names')
