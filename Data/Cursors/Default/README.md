<!-- SPDX-License-Identifier: Apache-2.0 -->

# Default cursor set

This directory contains project-authored geometric cursor artwork. `Source.json`
defines cursor roles, pixel coordinates, hotspots, and animation frame delays.
`Tools/Cursors/Generate.py` deterministically compiles these into `Default.acsr`
([ACSR v1 format](../../../Documentation/Architecture/Imaging.md)):

    python3 Tools/Cursors/Generate.py --source Data/Cursors/Default/Source.json --output Data/Cursors/Default/Default.acsr

The asset set provides 32x32 monochrome designs for the standard arrow, I-beam,
animated two-frame hourglass, four resize indicators, move cursor, pointing hand,
and prohibited symbol. Both the artwork definitions and generation script are
licensed under the project's Apache-2.0 license.
