<!-- SPDX-License-Identifier: Apache-2.0 -->

# Vegur coverage goldens

`VegurTests --record-goldens` generates 24 binary PGM masks for Light, Regular
and Bold at A24, V24, O24, g24, fi24, fl24, A12 and g36. Rasterization uses zero
fractional offset and an 8×8 coverage grid, without hinting. Source fonts and
hashes are in [Data/Fonts/Vegur](../../../../Data/Fonts/Vegur/NOTICE.md).

Tests compare complete files byte-for-byte, including dimensions and baseline
bearings. Synthetic cases independently check rectangle and reversed-contour
coverage. Normal CTest runs never overwrite goldens.

After intentional rasterization changes, regenerate explicitly and visually
check counters, descenders, ligatures, weight progression and edge coverage at
nearest-neighbour magnification. Review changed images before committing.
