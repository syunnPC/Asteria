# SPDX-License-Identifier: Apache-2.0
"""processor scheduling: measured intervals and certain-skew exclusion."""

def problems(record, cpus: int) -> list[str]:
    result = []
    expected = (1 << cpus) - 1
    if record.Published != expected:
        result.append('clock publication mask %#x, expected %#x' % (record.Published, expected))
    if record.Frequency <= 0 or record.EpsilonTicks <= 0:
        result.append('clock frequency/epsilon is not positive')
    for cpu, residual in enumerate(record.Processors):
        if cpu >= cpus:
            if residual.Flags:
                result.append('absent CPU %d has clock evidence' % cpu)
            continue
        if not residual.Flags & 1 or residual.Lower > residual.Upper:
            result.append('CPU %d lacks a valid residual interval' % cpu)
        if residual.Upper - residual.Lower != residual.RoundTrip:
            result.append('CPU %d residual width differs from round trip' % cpu)
        certain = residual.Lower > record.EpsilonTicks or residual.Upper < -record.EpsilonTicks
        excluded = bool(record.ExcludedMask & (1 << cpu))
        if excluded != (certain or not residual.Flags & (4 | 8)):
            result.append('CPU %d placement exclusion differs from its residual/invariant-counter/hypervisor gate' % cpu)
    if record.ExcludedMask & ~expected:
        result.append('clock exclusion names an absent processor')
    return result


def describe(record) -> str:
    return '; '.join('CPU %d: [%d, %d] ticks%s' % (cpu, item.Lower, item.Upper,
                    ' excluded' if record.ExcludedMask & (1 << cpu) else '')
                    for cpu, item in enumerate(record.Processors) if record.Published & (1 << cpu))
