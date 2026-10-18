# SPDX-License-Identifier: Apache-2.0
"""component launch: read the ABI's sole per-type capacity table."""
import re
from AsteriaVerify import Layout


def capacity(root, declaration, name):
    if name != 'Capacity':
        return None
    match = re.search(r'Capacity\{Abi::ObjectPoolCapacities\[static_cast<Foundation::Size>\(Abi::LimitedObject::(\w+)\)\]\}',
                      re.sub(r'\s+', '', declaration))
    if match is None:
        return None
    text = Layout.read(root / 'Source/Abi/ObjectCreationLimits.hpp')
    values = Layout.constants(text)
    order = Layout.enumerators(text, 'LimitedObject')
    table = re.search(r'ObjectPoolCapacities\[LimitedObjectCount\]\s*\{([^}]+)\}', text)
    if table is None:
        raise Layout.LayoutError('the ABI has no object capacity table')
    counts = tuple(Layout.evaluate(value, values) for value in table.group(1).split(','))
    if len(counts) != values['LimitedObjectCount'] or any(value is None for value in counts):
        raise Layout.LayoutError('the ABI object capacity table is malformed')
    return counts[order[match.group(1)]]
