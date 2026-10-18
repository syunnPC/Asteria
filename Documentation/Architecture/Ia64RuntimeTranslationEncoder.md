<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime translation encoder

`Ia64RuntimeTranslationEncoder` is a pure, allocation-free encoder for one
4 KiB page of a validated runtime `FaultFrameMappingPlan`. An accepted plan
contains at most 512 pages. `EncodePage` returns only the three operands needed
by a later software-refill insertion:

- the IA-64 translation word;
- a translation-insertion word selecting key zero and page-size encoding 12;
  and
- the page's faulting virtual address.

The active region register supplies RID identity; the encoder does not place a
RID in the translation word. It performs no privileged instruction and does
not read processor state.

Before producing operands, the encoder checks plan alignment and bounds,
reserved bytes, known frame kind, the 512-page limit, page index, arithmetic
wrap, and containment within one nonzero IA-64 virtual region. It validates the
consumed physical-width, implemented-virtual-address, page-size,
memory-attribute, and reserved capability fields. Physical addresses must fit
both the reported width and the 50-bit translation-word field.

Write-back, uncached, and write-combining policies are encoded only when the
capability mask supports them. Every mapping must be readable; writable and
executable permissions are mutually exclusive, and device mappings cannot be
executable. User mappings receive privilege level 3. Writable translations
set the dirty bit, and every successful translation sets present, accessed,
and exception-deferral bits.

Every failure returns an error without publishing or mutating state. A
successful result is still only a set of operands: it makes no publication,
ledger, refill-path, map-success, address-space activation, or runnable-root
claim. Those responsibilities belong to later transaction, refill, and
admission layers.
