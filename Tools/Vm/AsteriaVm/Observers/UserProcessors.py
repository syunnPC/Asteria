# SPDX-License-Identifier: Apache-2.0
"""processor scheduling: home ownership, CPL3 AP ticks and private idle roots."""
from pathlib import Path
import re


def _protocol(name):
    path = Path(__file__).resolve().parents[4] / 'Tests/Guest/Pager/Amd64SmpProtocol.h'
    match = re.search(r'^#define ' + name + r' (\w+)$', path.read_text(), re.M)
    if match is None:
        raise ValueError('missing SMP protocol constant ' + name)
    return int(match[1], 0)


SMP_TAG = _protocol('ASTERIA_SMP_TAG')


def worker_count():
    return _protocol('ASTERIA_SMP_WORKERS')


def problems(record, cpus, *, amd64=False, constants=None, pool=0, spinners=False):
    errors = []
    users = (1 << cpus) - 1 if amd64 else 1
    if spinners and record.Published != users:
        errors.append('user processor publication %#x, expected %#x' % (record.Published, users))
    if record.BootRanMask not in (0, 1) or record.SelfTestRanMask not in (0, 1):
        errors.append('boot or pager self-test ran away from CPU 0')
    if spinners and (record.SelfTestRanMask != 1 or record.CreatedRanMask != users):
        errors.append('self-test mask or created-thread processor coverage is incomplete')
    roots = set()
    for cpu, item in enumerate(record.Processors):
        if not users & (1 << cpu):
            if item.Dispatches or item.Ticks or item.CreatedDispatches or item.CreatedTicks:
                errors.append('CPU %d ran users despite its placement mask' % cpu)
            continue
        if spinners and (not item.Dispatches or not item.Ticks):
            errors.append('CPU %d lacks user entry or timer preemption' % cpu)
        if spinners and not all((item.CreatedDispatches, item.CreatedTicks, item.RunStops, item.Resolved)):
            errors.append('CPU %d lacks created-spinner preemption or fault resolution' % cpu)
        if amd64 and constants is not None:
            expected = constants.idle_root(pool, cpu)
            if item.IdleRoot != expected or item.IdleRoot in roots:
                errors.append('CPU %d idle root %#x, expected private %#x' % (cpu, item.IdleRoot, expected))
            roots.add(item.IdleRoot)
            if cpu and not item.IdleScrubVisits:
                errors.append('CPU %d never serviced its idle root' % cpu)
    spinner_mask = 0
    for slot, item in enumerate(record.Threads):
        if not item.ThreadIdentity:
            continue
        if item.Home >= cpus or (not amd64 and item.Home != 0):
            errors.append('slot %d has invalid home %d' % (slot, item.Home))
        if item.RanMask and item.RanMask != 1 << item.Home:
            errors.append('slot %d ran away from home %d' % (slot, item.Home))
        if item.SupervisionTag == 0 and item.Home:
            errors.append('slot %d pager self-test was placed on an AP' % slot)
        if item.SupervisionTag == SMP_TAG and item.Ticks and item.Faults:
            spinner_mask |= item.RanMask
    if spinners and spinner_mask != users:
        errors.append('shared-root spinners lack ticks/faults on every processor: %#x' % spinner_mask)
    return errors


def session_problems(record, cpus=0):
    """Desktop placement/ticks, independent of the four-CPU fault probe."""
    desktop = [item for item in record.Threads if item.ThreadIdentity
               and item.SupervisionTag not in (0, SMP_TAG)]
    errors = [] if any(item.Home and item.RanMask == 1 << item.Home for item in desktop) else [
        'no desktop component ran on an AP']
    if cpus:
        mask = (1 << cpus) - 1
        if record.Published != mask or record.CreatedRanMask != mask:
            errors.append('desktop processor coverage is incomplete')
        for cpu in range(1, cpus):
            item = record.Processors[cpu]
            if not all((item.CreatedDispatches, item.CreatedTicks, item.RunStops)):
                errors.append('CPU %d lacks created-user timer preemption' % cpu)
    return errors
