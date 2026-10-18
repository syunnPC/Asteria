# SPDX-License-Identifier: Apache-2.0
"""Q35 EHCIs reserved by a synthetic DMAR RMRR.

Check firmware diagnostics, kernel quarantine and the product console's
keyboard, pointer and display through the normal production driver.
"""
from __future__ import annotations

import re

from AsteriaVm import Inventory
from AsteriaVm.Drivers import Production


def kernel_problems(lines: list[str]) -> list[str]:
    """Both reserved EHCIs must be withheld from driver publication."""
    expected = ['ASTERIA:USB:SKIPPED:001A0:5:00', 'ASTERIA:USB:SKIPPED:001D0:5:00']
    problems = ['no %r' % line for line in expected if line not in lines]
    for device in ('1a', '1d'):
        if 'ASTERIA:USB:PUBLISHED:00%s0' % device.upper() in lines:
            problems.append('reserved EHCI 00:%s.0 was published' % device)
    return problems


REPORT = (
    re.compile(r'RMRR on 00:1a\.0 mask=0x01'),
    re.compile(r'RMRR on 00:1d\.0 mask=0x01'),
    re.compile(r'EHCI-RMRR 0000:00:1a\.0 8086:293c PMCSR=none command=0x[0-9a-f]{4} MSE=1 BME=[01] '
               r'BAR0=0x[0-9a-f]{8} bytes=0x[0-9a-f]{4}'),
    re.compile(r'EHCI-RMRR 0000:00:1d\.0 8086:293a PMCSR=none command=0x[0-9a-f]{4} MSE=1 BME=[01] '
               r'BAR0=0x[0-9a-f]{8} bytes=0x[0-9a-f]{4}'),
    re.compile(r'  CAPLENGTH=0x20 HCIVERSION=0x0100 HCSPARAMS=0x[0-9a-f]{8} N_PORTS=6 N_PCC=\d+ N_CC=\d+ '
               r'HCCPARAMS=0x[0-9a-f]{8} EECP=0x68'),
    re.compile(r'  USBLEGSUP@0x68=0x[0-9a-f]{8} BIOS-owned=[01] OS-owned=[01] USBLEGCTLSTS=0x[0-9a-f]{8} '
               r'SMI-enables=0x[0-9a-f]{4} SMI-status=0x[0-9a-f]{8}'),
    re.compile(r'  USBCMD=0x[0-9a-f]{8} RS=[01] PSE=[01] ASE=[01] USBSTS=0x[0-9a-f]{8} HCHalted=[01] PSS=[01] '
               r'ASS=[01] CONFIGFLAG=0x[0-9a-f]{8}'),
    re.compile(r'  SPECIAL_SMI unknown layout for 8086:293c'),
    re.compile(r'  SPECIAL_SMI unknown layout for 8086:293a'),
    re.compile(r'  RMRR#0 0x00000000feffc000-0x00000000feffffff UEFI map: .+'),
    re.compile(r'PCH SMI_EN LPC 8086:2918 unknown layout'),
)


def report_problems(text: str | None) -> list[str]:
    """Why REPORT.TXT lacks a line of the reserved-EHCI section."""
    if text is None:
        return ['EFI/ASTERIA/REPORT.TXT was not exported to the case ESP']
    lines = text.splitlines()
    return ['no line matches %r' % pattern.pattern for pattern in REPORT
            if not any(pattern.fullmatch(line) for line in lines)]


def run(context) -> None:
    Production.run(context)
    context.check_problems('ehci.kernel', kernel_problems(context.console.lines))
    report = Inventory.report_path(context.run_directory / 'Esp')
    context.check_problems('ehci.report', report_problems(report.read_text(errors='replace')
                                                          if report is not None else None))
