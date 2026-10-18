# SPDX-License-Identifier: Apache-2.0
"""Native launch authority and type-limit refusal evidence."""
from pathlib import Path
from AsteriaVerify import Layout


def problems(evidence, root, accounts, *, channels=0):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[4]
    order = Layout.enumerators(Layout.read(root / 'Source/Abi/ObjectCreationLimits.hpp'), 'LimitedObject')
    expected = [0] * order['Count']
    expected[order['WaitSet']] = accounts
    checks = (
        ('publication', evidence.Published, 1),
        ('created channels', evidence.Created, channels),
        ('duplicate live tags', evidence.TagRefusals, 0),
        ('Code flag refusals', evidence.CodeFlagRefusals, accounts),
        ('per-type refusals', list(evidence.TypeLimitRefusals), expected),
    )
    if channels:
        checks += (('destroyed channels', evidence.Destroyed, channels),
                   ('reclaimed channels', evidence.Reclaimed, channels),
                   ('live channels', evidence.Live, 0))
    result = ['%s: %r, expected %r' % (name, actual, wanted) for name, actual, wanted in checks if actual != wanted]
    if channels and (sum(evidence.Deliveries) < 13 or sum(evidence.Resolutions) != sum(evidence.Deliveries)
                     or sum(evidence.Terminations)):
        result.append('shared-root fault channel did not resolve every delivered fault')
    return result
