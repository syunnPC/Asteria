# SPDX-License-Identifier: Apache-2.0
"""Second supervisor, isolated fault delivery and teardown."""
from AsteriaVm import Guest
from AsteriaVm.Drivers.SessionProbe import _record


def run(context):
    context.wait_for_line('USER:# fault-channel: created supervisor received identity and resolved token, drained', 90)
    context.wait_for_line('USER:# root service: done', 90)
    Guest.wait(context)
    context.stop()
    record = _record(context, 'FaultChannelEvidence')
    channel = record.ApplicationLaunch
    smp = context.case.isa == 'Amd64' and context.case.cpus == 4
    count = 2 if smp else 1
    context.check('fault-channel.published', channel.Published == 1)
    context.check('fault-channel.collected', (channel.Created, channel.Destroyed, channel.Reclaimed, channel.Live) == (count, count, count, 0))
    context.check('fault-channel.delivery', sum(channel.Deliveries) >= 14 if smp else sum(channel.Deliveries) == 1)
    context.check('fault-channel.termination', sum(channel.Terminations) == 1 and sum(channel.Resolutions) == sum(channel.Deliveries))
    context.check('fault-channel.identity', sum(identity != 0 for identity in channel.ChannelIdentity) == count)
    domains = record.Components if context.case.isa == 'Amd64' else record.Domains
    context.check('fault-channel.domains-collected', domains.DomainsCreated == domains.DomainsReclaimed)
