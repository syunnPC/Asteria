# SPDX-License-Identifier: Apache-2.0
"""A test display incarnation withholds liveness Queries until init restarts it."""
import re
import struct

from AsteriaVm import Evidence, Guest, Amd64Layout
from AsteriaVm.Observers import Amd64Cpu, Amd64Paging
from AsteriaVerify.Elf import ElfFile


def run(context) -> None:
    from AsteriaVm.Observers import Amd64Evidence
    context.wait_for_line('USER:# watchdog: display-broker unresponsive for ', 90)
    stalled_path = context.artifact('WatchdogSpinnerEvidence.bin')
    problem = Evidence.save(context.qmp, context.console.lines, stalled_path)
    context.require('host.watchdog.spinner.kernel-evidence', problem is None, problem or '')
    stalled = Amd64Evidence.schema(context.target.root).decode(stalled_path.read_bytes())
    spinners = [thread for thread in stalled.UserProcessors.Threads
                if thread.SupervisionTag == 0x8000000000000100]
    context.check('host.watchdog.spinner.on-ap-preempted', len(spinners) == 1
                  and spinners[0].Home > 0 and spinners[0].Ticks > 0
                  and spinners[0].RanMask == 1 << spinners[0].Home, repr(spinners))
    context.wait_for_line('USER:# watchdog-test: display restarted and answering', 90)
    context.wait_for_line('USER:' + Guest.ROOT_DONE_LINE, 30)
    context.stop()
    reports = [re.fullmatch(r'USER:# watchdog: display-broker unresponsive for (\d+) ms', line)
               for line in context.console.lines]
    durations = [int(match[1]) for match in reports if match]
    context.check('host.watchdog.component.reports', len(durations) >= 3 and min(durations) >= 5000
                  and max(durations) >= 15000, repr(durations))
    snapshot = Amd64Cpu.capture(context.qmp, context.artifact('CpuState.elf'))
    memory = Amd64Paging.PageCache(lambda page: context.qmp.pmemsave(page, 4096))
    walker = Amd64Paging.PageWalker(memory)
    root = snapshot.cpus[0].cr3
    # Init may be suspended when CPU 0 is stopped; its root is retained by
    # the boot evidence rather than inferred from the current user thread.
    from AsteriaVm.Observers import Amd64Evidence
    path = context.artifact(Evidence.ARTIFACT)
    problem = Evidence.save(context.qmp, context.console.lines, path)
    context.require('host.watchdog.component.kernel-evidence', problem is None, problem or '')
    evidence = Amd64Evidence.schema(context.target.root).decode(path.read_bytes())
    constants = Amd64Layout.headers(context.target.root)
    root = constants.root(evidence.Runtime.PoolPhysicalStart, constants.pager_root_index)
    symbol = ElfFile(context.image('pager_elf')).symbol('AsteriaComponentWatchdogEvidence')
    page = walker.walk(root, symbol.value)
    context.require('host.watchdog.component.evidence-mapped', page is not None, 'root-owned watchdog evidence')
    words = struct.unpack('<6Q', context.qmp.pmemsave(page.physical, 48))
    context.check('host.watchdog.component.restarted-once', words[2] >= 15000 and words[3] >= 3
                  and words[4] == 1 and words[5] == 0, repr(words))
