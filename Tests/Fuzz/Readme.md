<!-- SPDX-License-Identifier: Apache-2.0 -->

# Font fuzz harnesses

- `FontParserFuzz`: Tests generated CFF and TrueType fonts with mutated table
  directories and nested tables; successful parses exercise bounded glyph outline
  loading and kern/liga shaping.
- `Type2Fuzz`: Tests mutated Type 2 CharString programs reused as local and global
  subroutines, verifying output-buffer canaries and operand/depth bounds.

Both harnesses run 20,000 deterministic seeded mutations across each supported
hosted profile and integrate with Clang 21 libFuzzer.
