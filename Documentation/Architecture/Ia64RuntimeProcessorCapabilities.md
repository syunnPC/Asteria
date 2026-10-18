<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 runtime processor capabilities

`Ia64RuntimeProcessorCapabilities` is a fixed four-processor, allocation-free
profile built from already-decoded `PalVirtualMemoryCapabilities` records. Its
caller supplies a nonzero admitted-processor mask and one array slot per
possible processor. Unadmitted slots are neither validated nor retained.

For every admitted processor, the profile retains the complete decoded record
unchanged for later processor-local validation. It also exposes a separate
common runtime subset containing only values that can safely govern all
admitted processors:

- physical-address width, implemented virtual-address MSB, and RID width are
  the minima;
- insertable page sizes, purgeable page sizes, and memory attributes are
  intersections; and
- hardware-walker availability is the logical AND.

The common subset is not a fabricated PAL record. In particular, unrelated
protection-key, translation-register, purge-count, tag, and cache-topology
fields remain available only in each exact processor record.

Initialization validates the consumed address ranges, page-size relationship,
memory-attribute encodings, and every retained reserved field. The resulting
common subset must retain 4 KiB insertion and purge plus write-back and
uncached memory. A failure changes no profile state and may be retried; after
one successful initialization the profile cannot be replaced.

## Publication boundary

The caller serializes initialization and publishes the successfully frozen
object with its platform release/acquire mechanism. The object must not be read
concurrently before that publication. After publication, admitted-mask,
processor-record, and common-subset queries are read-only, lock-free, and
return values rather than mutable aliases.

This profile performs no PAL call, processor startup, register installation,
translation operation, or address-space activation. Its admitted mask records
only which inputs participated in the reduction; it does not admit a processor
or address space for scheduling and does not establish a runnable root.
