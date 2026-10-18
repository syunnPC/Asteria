# SPDX-License-Identifier: Apache-2.0
"""Authored RMRR in WB BootServicesData, without a translating IOMMU.

The headless machine has no display master to preserve for scanout. The
production preemption probe runs the minimal user image, so this case tests
memory admission independently of resident graphics or DMA drivers.
"""
from AsteriaVm import Amd64Layout, Dmar, Evidence, Stages
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import Amd64Cpu, Amd64Evidence, MemoryImport


def run(context) -> None:
    context.wait_for_line(Stages.predicate(Amd64Default.LAUNCH_STAGE), Amd64Default.BOOT_SECONDS)
    loop = Amd64Cpu.idle_loop(context.target.tool('objdump'), context.image('user_elf'), Amd64Layout.USER_IDLE_SYMBOL)
    reached, detail = Amd64Cpu.wait_for_idle(context.qmp, context.artifact('CpuPoll.elf'), loop, context.seconds(60),
                                          abort=lambda: context.exited() or next(iter(context.console.failures()), None))
    context.check('memory-rmrr.user-idle', reached, detail)
    context.stop()
    path = context.artifact(Evidence.ARTIFACT)
    problem = Evidence.save(context.qmp, context.console.lines, path)
    context.require('memory-rmrr.evidence', problem is None, problem or '')
    record = Amd64Evidence.schema(context.target.root).decode(path.read_bytes())
    e = record.MemoryImport
    data = context.qmp.pmemsave(e.MapPhysical, e.MapRegions * MemoryImport.MAP_RECORD.size)
    context.artifact('MemoryMap.bin').write_bytes(data)
    context.check_problems('host.amd64.evidence.rmrr-bootservices', MemoryImport.rmrr_problems(
        record, data, Dmar.BOOTSERVICES_RMRR_BASE, Dmar.BOOTSERVICES_RMRR_LIMIT))
