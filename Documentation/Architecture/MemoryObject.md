<!-- SPDX-License-Identifier: Apache-2.0 -->

# Logical MemoryObject

`MemoryObject` is the architecture-neutral policy object for one logical range
of 4 KiB pages. It is distinct from `Frame`: a MemoryObject describes what a
pager may map, while a Frame identifies the physical backing selected for one
mapping. A mapping therefore requires independently authorized MemoryObject,
Frame, and AddressSpace generations, plus a DeviceRegion for device memory.

The immutable descriptor records the logical page count, maximum runtime page
permissions, cache policy, and one commitment kind: committed, discardable,
reconstructible, best effort, pinned, or device DMA. Every descriptor requires
read permission; individual mappings remain W^X even when the maximum policy
allows separate writable and executable mappings. Device-DMA policy never
authorizes execution.

Initialization pins the exact `ResourceDomain` generation and reserves kernel
metadata for every object. Committed, pinned, and device-DMA objects reserve
their complete committed-page guarantee; pinned and device-DMA objects also
reserve the complete resident-page guarantee. The pager still chooses physical
Frames and is responsible for populating or reconstructing content.

`FaultFrameMappingPreparation` checks the requested logical slice, protection,
and cache policy before publishing a plan. `AddressSpace` revalidates that plan
against the registered MemoryObject and retains one mapping reference together
with its MemoryObject, Frame, and any required DeviceRegion pins. `Quiesce`
cannot release the resource
guarantee while any mapping reference remains. After every using AddressSpace
has quiesced, the object releases its reservation and domain pin and becomes
terminal.
