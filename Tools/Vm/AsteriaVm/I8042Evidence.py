# SPDX-License-Identifier: Apache-2.0
"""AML boot facts: namespace publication and floating-port refusal observations."""
from __future__ import annotations


def record(context) -> None:
    if not context.case.reaches_userspace:
        return
    # IA-64's synthetic user probes (and the native preemption probe built on
    # them) start no pager, which is where IA-64 publishes the controller.
    if context.case.isa == 'Ia64' and context.target.option('ia64_synthetic_user_probe') == '1':
        return
    lines = context.console.lines
    if context.case.floating_keyboard:
        context.check('i8042.floating', 'ASTERIA:I8042:UNPUBLISHED' in lines
                      and 'ASTERIA:I8042:REASON:port-0x64-floating' in lines,
                      'authored SSDT supplied through -acpitable; declaration cannot override a floating port')
    elif not context.case.i8042:
        context.check('i8042.no-evidence', 'ASTERIA:I8042:UNPUBLISHED' in lines
                      and 'ASTERIA:I8042:REASON:no-evidence' in lines,
                      'i8042=off continues without controller handles')
    elif not context.case.fadt8042:
        context.check('i8042.aml', 'ASTERIA:I8042:PUBLISHED:AML-PNP0303' in lines,
                      'NOFADT8042 masks FADT bit 1; QEMU namespace keyboard supplies publication evidence')
    else:
        context.check('i8042.fadt', 'ASTERIA:I8042:PUBLISHED:FADT-8042' in lines,
                      'FADT declaration and a non-floating status publish four controller handles')
