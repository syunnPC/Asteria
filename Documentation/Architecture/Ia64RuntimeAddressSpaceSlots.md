<!-- SPDX-License-Identifier: Apache-2.0 -->

# IA-64 run-time AddressSpace slots

`Ia64RuntimeAddressSpaceSlots` is IA-64's `IAddressSpaceProvider`: the
AddressSpaces ObjectCreate type 2 creates
([domain creation ABI](SystemCallAbi.md)) and ObjectDestroy
reclaims ([object retirement](SystemCallAbi.md#object-management-semantics)).
It has sixteen slots, matching `AddressSpacePool`. Each slot holds a private
eight-RID receipt, an [`Ia64RuntimeAddressSpacePageTable`](Ia64RuntimeAddressSpacePageTable.md)
seeded with the same canonical Region-0 seed as the boot roots, and an
`AddressSpace` over it: 76,264 bytes. `Ia64BspRuntimeMemory` embeds the slots, so every
backend's region set and ledger lie in the runtime-object corridor the
binding table admits, and hands them out once through
`ConfigureRuntimeAddressSpaces`, after `ConfigureTranslationCachePurge`.
The corridor lies in the run-time memory reservation, one page that one
locked DTR maps whatever the slot count
([capacity](Ia64BspRuntimeMemory.md#slot-capacity-and-the-translation-register-budget)),
so the page's size bounds the slots; RIDs (eight per incarnation from a
2^18-or-wider space, never reused) and ledgers (256 records per incarnation,
rebuilt on release) do not.

Unlike the boot roots' [owner](Ia64RuntimeAddressSpaceOwner.md), a slot
registers and pins nothing: `AddressSpacePool` registers the space, binds its
identity and mints its capability, and its collection waits for every pin to
drop, so a pin of the provider's would keep the slot forever.

RIDs and ledgers are single-use, so every incarnation takes a fresh receipt
from the boot's one RID allocator, and a failure after the receipt loses it.
`Configure` therefore proves once what the backend and the AddressSpace check
on every initialization: the common capabilities and admitted processors
exist, the preparation's plan is the canonical seed
(`Ia64RuntimeAddressSpacePageTable::IsCanonicalSeed`) and the allocator is Ready
with the common RID width. `InitializeSlot` then checks, before it allocates,
the slot free, the allocator Ready with that width and room for another block,
and the backend and AddressSpace unused. It initializes the backend, names the
runtime memory's translation-cache purge for it, and initializes the
AddressSpace with the preparation's exact plan pointer (the backend's `Claim`
requires it); the space is returned unregistered. Should a step after the
receipt fail all the same, the slot is spent, as the pool spends it.

`ReleaseSlot` requires the AddressSpace `Quiesced` and the backend `Retired`.
Quiesce releases the backend, which refuses while its ledger is bound on any
processor; a processor binds its next root's ledger before it leaves a root,
and the pool collects only a space resident nowhere, so this holds. The slot
then constructs both afresh in place and forgets the receipt; the dead RIDs
never return, so their cached translations need no purge. That covers a space
a ResourceDomain drain destroys with its mappings still in it
([resource accounting](ResourceDomain.md)): its
frames may be handed out again while translations under its RIDs linger, but
nothing can reach them. The pool faults on any other refusal of `Quiesce`.

The snapshot counts free slots, preparations, releases and spent slots;
`Ia64BspRuntimeMemory::GetAllocatedAddressSpaceCount` reports the allocator's
count of every root ever prepared. Hosted tests drive the slots through the
kernel's `AddressSpacePool`: one fresh RID block per creation, every slot
held before the full pool refuses a creation and takes none, a destroyed
space's slot released and built again with new RIDs, and the charge returned.
