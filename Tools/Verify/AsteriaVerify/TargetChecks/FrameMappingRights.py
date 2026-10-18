# SPDX-License-Identifier: Apache-2.0
"""CPU Frame mapping inventory.

BlockMapRight covers AHCI's DMA-only client use.
Client CPU copies require Map|Write; DMA and register CPU mappings use self-created Frames.
The source property inventories wrappers and all their callers; adding a CPU
mapping site requires classifying its Frame's provenance and authority here.
Watchdog recovery reuses the reviewed ComponentLoader mapping sites; its
private reply Endpoints introduce no additional CPU Frame mappings.
"""
from __future__ import annotations

import collections
import dataclasses
import pathlib
import re
import sys

from AsteriaVerify.Checks import Property, register

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / 'Mutation'))
from AsteriaMutation.Tokenizer import tokenize, COMMENT, DIRECTIVE, STRING, CHARACTER

MAP_NAMES = {'MapWindow', 'MapMemory', 'FaultResolve'}
CONTROL = {'if', 'while', 'for', 'switch', 'catch', 'sizeof', 'alignof', 'noexcept', 'static_assert'}


def sites(source: str) -> dict[tuple[str, str], int]:
    tokens = [t.text for t in tokenize(source) if t.kind not in (COMMENT, DIRECTIVE, STRING, CHARACTER)]
    stack, pairs = [], {}
    for i, t in enumerate(tokens):
        if t in ('(', '{'):
            stack.append(i)
        elif t in (')', '}') and stack:
            first = stack.pop()
            pairs[first] = i
    functions = []
    for i, t in enumerate(tokens[:-1]):
        if tokens[i + 1] != '(' or not re.fullmatch(r'[A-Za-z_]\w*', t) or t in CONTROL:
            continue
        end = pairs.get(i + 1, i + 1) + 1
        while end < len(tokens) and tokens[end] in ('const', 'noexcept', 'override', 'final'):
            end += 1
        if end < len(tokens) and tokens[end] == '{' and end in pairs:
            name, start = t, i
            while start >= 2 and tokens[start - 1] == '::':
                start -= 2
                name = tokens[start] + '::' + name
            functions.append((end, pairs[end], name))
    found = collections.Counter()
    for i, t in enumerate(tokens[:-1]):
        raw = i >= 2 and tokens[i - 2:i] == ['SystemCallNumber', '::'] and t in ('MemoryMap', 'FaultResolve')
        call = tokens[i + 1] == '(' and (t in MAP_NAMES or (t == 'Map' and i > 0 and tokens[i - 1] in ('.', '->')))
        if not (raw or call):
            continue
        owners = [f for f in functions if f[0] < i < f[1]]
        if owners:
            owner = min(owners, key=lambda f: f[1] - f[0])[2]
            found[owner, t + ' syscall' if raw else t] += 1
    return dict(found)


# Filled from the reviewed source below: (file, function, entry, occurrences)
# -> (Frame provenance, rights source, mapping access). Both read-only peers
# and every writer are named, including AHCI copies, fault helpers and UI-font tail mappings.
# Include UI fonts, fault helpers and native producer pools; all producer
# storage stays RW.
INVENTORY = {
    ('Source/User/Pager/ProductLogin.cpp', 'ProductLogin::Start', 'Map', 1): ('root-owned nonsecret limiter checkpoint', 'CreatedRights', 'RW; unmapped before child starts'),
    ('Source/User/Pager/ProductLogin.cpp', 'ProductLogin::CoverDeadDisplay', 'MapWindow', 1): ('root-retained scanout DeviceRegion after broker drain', 'DeviceRegion Map; root-created MMIO Frame', 'RW uncached fixed blank cover'),
    ('Source/User/Identity/AuthService.cpp', 'AuthService::Map', 'MapMemory', 1): ('auth-created arena; root fixture tail/limiter; login secret page', 'CreatedRights; fixture/password Map; limiter Map|Write', 'RW arena/limiter; R fixture/password; password unmapped before Copied'),
    ('Source/User/Identity/LoginService.cpp', 'LoginService::Allocate', 'MapMemory', 1): ('self-created public pixels and secret submission page', 'CreatedRights', 'RW'),
    ('Source/User/Pager/ProductDesktop.cpp', 'ProductDesktop::Pools', 'MapMemory', 1): ('root-owned boot-log pools', 'CreatedRights', 'RW'),
    ('Tests/Guest/Pager/Amd64SmpScenarios.cpp', 'RunAmd64SmpScenarios', 'Map', 2): ('supervisor-created shared control/lazy RAM', 'CreatedRights', 'RW'),
    ('Tests/Guest/Pager/Amd64SmpScenarios.cpp', 'Resolve', 'FaultResolve', 1): ('supervisor-created lazy RAM', 'CreatedRights', 'RW'),
    ('Source/User/Component/AhciDriver.cpp', 'CopyClient', 'MapWindow', 1): ('block client domain', 'BlockMapRight', 'RW'),
    ('Source/User/Component/AhciDriver.cpp', 'PrepareTransfer', 'MapWindow', 1): ('block client domain', 'BlockMapRight', 'RW'),
    ('Source/User/System/SystemCallClient.cpp', 'SystemCallClient::MapMemory', 'MemoryMap syscall', 1): ('caller request', 'caller', 'caller'),
    ('Source/User/System/SystemCallClient.cpp', 'SystemCallClient::FaultResolve', 'FaultResolve syscall', 1): ('caller request', 'caller', 'caller'),
    ('Source/User/System/SystemCallClient.cpp', 'SystemCallClient::FaultTerminate', 'FaultResolve syscall', 1): ('no Frame (Terminate)', 'token Invoke', 'none'),
    ('Source/User/Component/ComponentProgram.cpp', 'UseAccount', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Component/DisplayBroker.cpp', 'MapScanoutPages', 'MapWindow', 1): ('self-created device Frame', 'CreatedRights', 'RW'),
    ('Source/User/Session/ClientConnection.cpp', 'MapOwned', 'MapMemory', 1): ('self-created toolkit client pool RAM', 'CreatedRights', 'RW'),
    ('Source/User/Toolkit/SessionComponent.cpp', 'Map', 'MapMemory', 1): ('root font tail (R) or self-created font workspace (RW)', 'attenuated Map (font); CreatedRights (workspace)', 'caller-selected R/RW'),
    ('Source/User/Component/DisplaySession.cpp', 'MapRead', 'MapMemory', 1): ('session or enrolled login producer domain', 'SessionMapRight / Seat Frame Map', 'R'),
    ('Source/User/Component/SessionCompositor.cpp', 'MapRead', 'MapMemory', 1): ('session client domain', 'SessionMapRight', 'R'),
    ('Source/User/Component/SessionCompositor.cpp', 'ProbePoolWrite', 'MapWindow', 1): ('session client pool or root UI-font tail (Frame write-authority probe)', 'attenuated Map', 'denied RW with writable MemoryObject'),
    ('Source/User/Component/SessionCompositor.cpp', 'SessionCompositor::LoadUiFonts', 'MapMemory', 1): ('root-filled immutable tail of the compositor stack allocation', 'attenuated Map', 'R'),
    ('Source/User/Component/SessionCompositor.cpp', 'MapOutput', 'MapMemory', 1): ('self-created output RAM', 'CreatedRights', 'RW'),
    # The broad.Map inventory also sees WindowManager's visibility operation.
    ('Source/User/Component/SessionCompositor.cpp', 'SessionCompositor::Request', 'Map', 1): ('no Frame (window visibility)', 'none', 'none'),
    ('Source/User/Component/DriverSetup.hpp', 'MapRegisters', 'MapWindow', 1): ('self-created device Frame', 'CreatedRights', 'RW'),
    ('Source/User/Component/DriverSetup.hpp', 'MapDmaPage', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Component/FilesystemService.cpp', 'MapClient', 'MapWindow', 1): ('filesystem client domain', 'FsDataRights', 'RW'),
    ('Source/User/Component/FilesystemService.cpp', 'PrepareWindow', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Component/NetworkService.cpp', 'PrepareWindow', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Component/NicDriver.cpp', 'MapClient', 'MapWindow', 1): ('network service domain', 'NicMapRight', 'RW'),
    ('Source/User/Component/UsbHostDriver.cpp', 'MapBlock', 'MapWindow', 1): ('self-created device Frame', 'CreatedRights', 'RW'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::Load', 'Map', 1): ('root-created shared image domain', 'attenuated Map', 'R/RX'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::MapWindow', 'Map', 1): ('caller (enumerated above)', 'caller', 'RW'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::StageSharedSegment', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::LoadSegment', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::LoadSegment', 'Map', 1): ('self-created RAM', 'CreatedRights / attenuated Map', 'RW/R/RX (private or immutable)'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::LoadData', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Pager/ComponentLoader.cpp', 'ComponentLoader::LoadData', 'Map', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Pager/DeviceInventory.cpp', 'ReadDeviceInventory', 'Map', 1): ('kernel boot inventory', 'Map|Read', 'R'),
    ('Source/User/Pager/DeviceManagerRole.cpp', 'DeviceManagerRole::BeginQuiesce', 'MapWindow', 1): ('self-created device Frame', 'CreatedRights', 'RW'),
    ('Source/User/Pager/DeviceManagerRole.cpp', 'DeviceManagerRole::BeginUsbHost', 'MapWindow', 1): ('self-created device Frame', 'CreatedRights', 'RW'),
    ('Source/User/Pager/ObjectCalls.cpp', 'ObjectCalls::Map', 'MapMemory', 1): ('caller (enumerated above)', 'caller', 'caller'),
    ('Source/User/Pager/PagerProgram.cpp', 'PagerProgram::ResolveDelivery', 'FaultResolve', 1): ('kernel boot lazy Frame', 'boot Map|Write', 'RW'),
    ('Source/User/Pager/PagerProgram.cpp', 'PagerProgram::TestMapping', 'MapWindow', 2): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/Pager/PagerProgram.cpp', 'PagerProgram::MapWindow', 'MapMemory', 1): ('caller (enumerated above)', 'caller', 'RW'),
    ('Source/User/Pager/PagerProgram.cpp', 'PagerProgram::TestDomain', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Source/User/System/DeviceAccess.hpp', 'MapWindow', 'MapMemory', 1): ('caller (enumerated above)', 'caller', 'RW'),
    ('Tests/Guest/Pager/BlockDemo.cpp', 'MapData', 'Map', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Tests/Guest/Pager/FrameRightsDemo.cpp', 'TestRootService::RunFrameRights', 'Map', 3): ('self-created RAM', 'CreatedRights / attenuated Map', 'RW, denied RW, R'),
    ('Tests/Guest/Pager/Amd64ReturnScenarios.cpp', 'TestRootService::RunAmd64ReturnBoundary', 'Map', 2): ('self-created RAM', 'CreatedRights', 'RW staging, unmap, RX boundary'),
    ('Tests/Guest/Pager/FsDemo.cpp', 'MapData', 'Map', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Tests/Guest/Pager/Ia64FaultScenarios.cpp', 'ResolveProbe', 'FaultResolve', 1): ('self-created RAM', 'CreatedRights', 'RW/RX'),
    ('Tests/Guest/Pager/Ia64FaultScenarios.cpp', 'RunLazyProbe', 'Map', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Tests/Guest/Pager/Ia64FaultScenarios.cpp', 'RunSpinnerProbe', 'Map', 2): ('self-created RAM', 'CreatedRights', 'RW/RX'),
    ('Tests/Guest/SessionProbe.cpp', 'Run', 'MapWindow', 1): ('self-created RAM', 'CreatedRights', 'RW'),
    ('Tests/Guest/PatternClient.cpp', 'MapProducer', 'MapMemory', 1): ('self-created native producer pools', 'CreatedRights', 'RW'),
    ('Tests/Guest/Pager/CompositorDemo.cpp', 'ArenaMaxima', 'MapMemory', 1): ('self-created maximum-layout RAM', 'CreatedRights', 'RW'),
    ('Tests/Guest/PatternClient.cpp', 'Initialize', 'MapMemory', 1): ('self-created producer RAM', 'CreatedRights', 'RW'),
}


@dataclasses.dataclass(frozen=True)
class Facts:
    sites: dict
    rights: dict
    guards: dict


@register
class FrameMappingRights(Property):
    variants = ('firmware',)

    def extract(self, context, subject):
        found = {}
        for directory in ('Source/User', 'Tests/Guest'):
            for path in sorted((context.root / directory).rglob('*')):
                if path.suffix in ('.cpp', '.hpp'):
                    for (function, entry), count in sites(path.read_text()).items():
                        found[path.relative_to(context.root).as_posix(), function, entry, count] = True
        rights = {}
        for file, name in (('FsFrame', 'FsDataRights'), ('NetFrame', 'NicMapRight'), ('BlockFrame', 'BlockMapRight'),
                           ('SessionFrame', 'SessionMapRight')):
            path = context.root / ('Source/User/' + ('Session/' if file == 'SessionFrame' else 'Component/') + file + '.hpp')
            if path.exists():
                match = re.search(r'\b' + name + r'\s*\{(0x[0-9a-fA-F]+)U\}', path.read_text())
                if match:
                    rights[name] = int(match[1], 16)
        guards = {}
        for isa in ('Amd64', 'Ia64'):
            source = (context.root / ('Source/Platform/%s/System/%sPagerRuntime.cpp' % (isa, isa))).read_text()
            guards[isa + ' lazy Write'] = bool(re.search(
                r'MintInto\(Abi::PagerBootstrapLazyFrameWord,\s*frame.GetValue\(\),\s*map.With\(CapabilityRight::Write\)\)', source))
        source = (context.root / 'Source/Kernel/Memory/FrameObjectPool.hpp').read_text()
        created = source.split('CreatedRights{', 1)[1].split(';', 1)[0]
        for right in ('Read', 'Write', 'Map'):
            guards['creator ' + right] = 'CapabilityRight::' + right in created
        readonly = (context.root / 'Source/User/Component/DisplaySession.cpp').read_text()
        guards['session output read-only'] = 'ReadUser{0x09U}' in readonly
        compositor = (context.root / 'Source/User/Component/SessionCompositor.cpp').read_text()
        guards['compositor pools read-only'] = 'ReadUser{0x09U}' in compositor
        font_body = compositor.split('bool SessionCompositor::LoadUiFonts(', 1)[1].split('bool SessionCompositor::InputCredits(', 1)[0]
        guards['font mapping read-only'] = 'request[Abi::FaultResolvePermissionsOffset] = ReadUser;' in font_body
        inventory = (context.root / 'Source/User/Pager/DeviceInventory.cpp').read_text()
        guards['inventory read-only'] = 'ReadPermissions{9U}' in inventory
        helper = (context.root / 'Source/User/System/DeviceAccess.hpp').read_text()
        guards['MapWindow writable'] = 'ReadWriteUser{0x0BU}' in helper
        loader = (context.root / 'Source/User/Pager/ComponentLoader.cpp').read_text()
        guards['shared image attenuation'] = bool(re.search(
            r'DeriveCapability\(handle,\s*Abi::CapabilityRightMap,', loader))
        font_grants = loader.split('bool ComponentLoader::PrepareUiFonts(', 1)[1].split('bool ComponentLoader::PrepareSession(', 1)[0]
        font_grants = re.sub(r'\s+', '', font_grants)
        for kind in ('Frame', 'MemoryObject'):
            guards['font ' + kind + ' Map-only grant'] = bool(re.search(
                r'GrantCapability\(mapping\.' + ('Frame' if kind == 'Frame' else 'Memory') + r',component.Capabilities,\s*MapRight,Abi::ObjectType' + kind, font_grants))
        return Facts(found, rights, guards)

    def verify(self, facts):
        problems = ['unreviewed CPU mapping site: %s' % (site,) for site in facts.sites.keys() - INVENTORY.keys()]
        problems += ['missing reviewed CPU mapping site: %s' % (site,) for site in INVENTORY.keys() - facts.sites.keys()]
        for name, expected in (('FsDataRights', 0x0118), ('NicMapRight', 0x0018), ('BlockMapRight', 0x0018), ('SessionMapRight', 0x0010)):
            if facts.rights.get(name) != expected:
                problems.append('%s must carry %#x for its reviewed consumer' % (name, expected))
        problems += [name + ' is absent' for name, present in facts.guards.items() if not present]
        return problems

    def mutants(self, facts):
        for site in facts.sites:
            yield 'lost site %s' % (site,), dataclasses.replace(facts, sites={k: v for k, v in facts.sites.items() if k != site})
        yield 'unreviewed writable client', dataclasses.replace(facts, sites={**facts.sites, ('new.cpp', 'Writer', 'MapWindow', 1): True})
        for name in facts.rights:
            yield 'changed ' + name, dataclasses.replace(facts, rights={**facts.rights, name: facts.rights[name] ^ 8})
        for name in facts.guards:
            yield 'missing ' + name, dataclasses.replace(facts, guards={**facts.guards, name: False})

    def summary(self, facts):
        return '%d reviewed CPU mapping entries; filesystem/NIC/AHCI writers and read-only session/UI-font peers' % len(facts.sites)
