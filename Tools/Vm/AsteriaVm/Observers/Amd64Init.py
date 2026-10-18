# SPDX-License-Identifier: Apache-2.0
"""Resident product init evidence, including the pager's retained management self-test.

Unlike scenario teardown, init keeps its display and optional input/PS2 domains,
image mappings, threads, endpoints and interrupt bindings. Counts follow the
loader's staged shared segments and private pages in the linked component ELF.
"""
from __future__ import annotations

from AsteriaVerify import Layout


def problems(record, constants, expectations, i8042, frame_pool, root) -> dict[str, list[str]]:
    residents = 3 if i8042 else 1
    # Watchdog.md: display and the optional Input Broker each retain one
    # private root-owned liveness reply Endpoint beside their existing links.
    endpoints = (7 if i8042 else 2) + 1 + int(i8042)
    images, image_pages = dict(expectations.images), dict(expectations.image_pages)
    display_filled, display_zeroed = images['display']
    component_filled, component_zeroed = images['component']
    # Display stages and retains its own image once; only input and PS/2 share the component image's
    # private layout.
    filled = display_filled + component_filled * int(i8042)
    resident_segments = display_filled + display_zeroed + (residents - 1) * (component_filled + component_zeroed)
    frames = filled + display_zeroed + (residents - 1) * component_zeroed + residents + 1
    pages = image_pages['display'] + image_pages['component'] * int(i8042) + expectations.display_private_pages \
        + (residents - 1) * expectations.private_pages + residents * (constants.component_stack_pages + 1)
    result = {}

    def exact(name, evidence, wanted):
        errors = ['%s is %#x, expected %#x' % (field, evidence[field], value)
                  for field, value in wanted.items() if evidence[field] != value]
        result['host.amd64.evidence.' + name] = errors
        return errors

    exact('supervision', record.Supervision, {'Published': 1, 'Terminations': 2,
          'SpinnerExited': 0, 'SpinnerExitCode': 0, 'Deferrals': 0})
    c = record.Components
    errors = exact('components', c, {'Published': 1, 'AddressSpacesCreated': residents,
        'AddressSpacesLive': residents, 'AddressSpacesReclaimed': 0,
        'AddressSpacesFree': constants.domain_slots - residents,
        'CapabilitySpacesCreated': residents, 'CapabilitySpacesLive': residents,
        'CapabilitySpacesReclaimed': 0, 'CapabilitySpacesFree': constants.domain_slots - residents,
        'DomainsCreated': residents, 'DomainsDestroyed': 0, 'DomainsReclaimed': 0,
        'TaggedFaults': 0, 'LastTag': 0, 'DomainExits': 0, 'ArenasPrepared': residents,
        'ResourceDomainsCreated': residents, 'ResourceDomainsDestroyed': 0,
        'ResourceDomainsReclaimed': 0, 'ResourceDomainsLive': residents})
    if min(c.AddressSpaceBytes, c.CapabilitySpaceBytes, c.WaitExpiries) <= 0:
        errors.append('resident spaces and supervisor waits must have nonzero evidence')
    t = record.Threads
    errors = exact('threads', t, {'Published': 1, 'Created': 6 + residents, 'Destroyed': 6,
        'Reclaimed': 6, 'Exits': 2, 'Reaped': 6, 'Lines': 1,
        'LastExitCode': constants.killed_exit_code,
        'FreeSlots': constants.thread_slots - residents,
        'FreeNativeSlots': constants.thread_slots - residents,
        'ThreadsUsage': residents, 'Faults': 2, 'Deferrals': 0, 'Exceptions': 1, 'Kills': 2})
    if t.SlotMetadataBytes < constants.kernel_stack_pages * 4096 or min(t.RunStops, t.DestroyPolls) <= 0:
        errors.append('self-test threads must preempt, refuse destruction and charge native stack storage')
    s = record.Services
    errors = exact('services', s, {'Published': 1, 'WaitSetsCreated': residents + 1,
        'WaitSetsReclaimed': 1, 'Binds': 7 if i8042 else 1,
        'WaitExpiries': 1, 'WaitAborts': 1, 'AbortedExits': 1})
    if s.WaitsBlocked < residents + 1:
        errors.append('resident services and the self-test must block through WaitSets')
    exact('interrupts', record.Interrupts, {'Published': 1, 'Created': 2 * int(i8042),
        'Destroyed': 0, 'Reclaimed': 0, 'Live': 2 * int(i8042), 'Delivered': 0, 'Spurious': 0})
    # The product inventory reader creates, maps, unmaps and destroys one
    # MemoryObject over the immutable bootstrap Frame, before starting input.
    m = record.ObjectManagement
    queue = Layout.constants((root / 'Source/Kernel/Ipc/Endpoint.hpp').read_text())['MaximumQueueEntries']
    wire = Layout.constants((root / 'Source/Kernel/Ipc/MessageCodec.hpp').read_text())['MaximumWireSizeBytes']
    errors = exact('object-management', m, {'Published': 1, 'Created': endpoints + 2,
        'Destroyed': 2, 'Reclaimed': 2, 'OccupiedSlots': endpoints + 2 * frames,
        'IpcQueueBytes': endpoints * queue * wire, 'FramesCreated': frames + 2, 'FramesReclaimed': 2,
        'MemoryObjectsCreated': frames + filled + 3, 'MemoryObjectsReclaimed': filled + 3,
        'CommittedPages': pages, 'ResidentPages': pages,
        'AllocatorCleanPages': m.AllocatorCleanPages, 'AllocatorDirtyPages': m.AllocatorDirtyPages,
        'Mappings': 2 + filled + resident_segments + 2 * residents + 2,
        'Unmappings': 2 + filled + residents,
        'MappedVirtualStart': constants.component_stack_start, 'MappedPages': constants.component_stack_pages + 1})
    if m.AllocatorCleanPages + m.AllocatorDirtyPages != record.MemoryImport.ManagedPages - pages:
        errors.append('imported free-page partition differs from managed minus resident Frames')
    floor = (c.AddressSpacesLive * c.AddressSpaceBytes + c.CapabilitySpacesLive * c.CapabilitySpaceBytes
             + frames * (m.FrameMetadataBytes + m.MemoryObjectMetadataBytes)
             + endpoints * m.EndpointMetadataBytes + residents * t.SlotMetadataBytes)
    if m.MetadataBytes < floor or m.PeakMetadataBytes != m.MetadataBytes:
        errors.append('metadata must cover all resident objects and remain charged at its peak')
    if min(m.FrameMetadataBytes, m.MemoryObjectMetadataBytes, m.EndpointMetadataBytes, m.RegisteredObjects) <= 0:
        errors.append('management objects must be registered with nonzero metadata charges')
    low, high = frame_pool
    if not low <= m.MappedPhysicalStart < high - m.MappedPages * 4096 + 1:
        errors.append('last component stack mapping must lie inside the frame pool')
    return result
