<!-- SPDX-License-Identifier: Apache-2.0 -->

# Typed IPC Wire Format

## Scope

The typed-IPC boundary fixes a language-neutral envelope and its canonical encoder and decoder. The bounded Endpoint
queue uses that codec and the capability layer's atomic move/copy transaction.
Message copying, endpoint-capability authorization, scheduler blocking and
call/reply policy belong to the syscall dispatcher and preserve these wire checks.

Every multi-byte integer is unsigned little-endian. No external byte range is overlaid with a C++ object, and no
field offset depends on compiler padding, a vtable, RTTI, or an STL representation. One complete frame is at most 504
bytes:

```text
88-byte header
+ at most 4 × 8-byte variable-payload descriptors
+ at most 4 × 32-byte capability attachments
+ at most 256 payload bytes
```

The in-kernel normalized `MessageEnvelope` is a separate 496-byte value. It is not itself a wire ABI.

The C++ boundary carries every byte range as one value: `ReadOnlyByteView { Data, SizeBytes }` for decoding and
`WritableByteView { Data, SizeBytes }` for encoding. Both views are non-owning and 16 bytes on supported 64-bit
targets. Encoding returns `Foundation::Result<Foundation::Size, MessageCodecError>`; a successful value is the exact
number of bytes written and a failure contains no usable size.

## Header version 1

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic, bytes `AIPC` |
| 4 | 2 | wire version, exactly 1 |
| 6 | 2 | header size, exactly 88 |
| 8 | 4 | total frame size |
| 12 | 2 | total payload size |
| 14 | 2 | fixed payload prefix size |
| 16 | 8 | interface identifier low word |
| 24 | 8 | interface identifier high word |
| 32 | 4 | method identifier |
| 36 | 2 | interface major version |
| 38 | 2 | interface minor version |
| 40 | 8 | nonzero transaction identifier |
| 48 | 8 | monotonic deadline ticks |
| 56 | 8 | trace identifier high word |
| 64 | 8 | trace identifier low word |
| 72 | 8 | span identifier |
| 80 | 1 | variable-payload descriptor count |
| 81 | 1 | capability attachment count |
| 82 | 1 | message kind |
| 83 | 1 | cancellation state |
| 84 | 4 | reserved, zero |

The interface identifier, method identifier, interface major version, and transaction identifier are nonzero. A
trace context is either wholly absent or has both a nonzero 128-bit trace identifier and a nonzero span identifier.
The deadline is carried as an absolute monotonic value; expiry is checked during dispatch, not during byte
decoding. Endpoint checks it once, against the shared clock when the message is sent, and rejects an expired message
there. A queued message whose deadline passes before it is received is still delivered with its deadline unchanged;
the receiver compares that deadline with its own clock.

Message kind values are 1 `SynchronousCall`, 2 `Reply`, 3 `Asynchronous`, and 4 `Cancellation`. Cancellation-state
values are 0 `None`, 1 `Cancellable`, and 2 `Requested`. Calls and asynchronous messages cannot use `Requested`;
replies must use `None`; a cancellation message must use `Requested` and carry no payload, variable descriptor, or
capability attachment.

## Variable payload descriptors

Each descriptor is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | offset from the start of payload |
| 2 | 2 | byte count |
| 4 | 4 | reserved, zero |

Descriptor order is the IDL field order. The first descriptor starts exactly at the fixed-prefix size, every later
descriptor starts exactly where its predecessor ends, and the final descriptor ends exactly at total payload size.
This rejects gaps, overlap, hidden payload bytes, and alternate encodings of the same nonempty field sequence.
Zero-length fields are retained as ordered descriptors and are allowed.

## Capability attachments

Each attachment is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | handle selector |
| 8 | 8 | handle authenticator |
| 16 | 8 | requested lease deadline |
| 24 | 2 | requested attenuated rights |
| 26 | 1 | transfer mode: 1 move, 2 copy |
| 27 | 1 | expected object type |
| 28 | 4 | reserved, zero |

Selector zero, an empty or unknown rights set, an invalid object type, an unknown transfer mode, and duplicate source
selectors are rejected. The authenticator may be zero because selector zero alone is the invalid-handle sentinel.
Rights retain the bit assignments in `CapabilityTypes.hpp`.

Object-type wire values are explicit and shared with the syscall query ABI:

| Value | Type | Value | Type |
|---:|---|---:|---|
| 1 | ProtectionDomain | 11 | Notification |
| 2 | AddressSpace | 12 | WaitSet |
| 3 | CapabilitySpace | 13 | Timer |
| 4 | Thread | 14 | Interrupt |
| 5 | SchedulingContext | 15 | DeviceRegion |
| 6 | ResourceDomain | 16 | IommuDomain |
| 7 | MemoryObject | 17 | FaultChannel |
| 8 | Frame | 18 | CpuSet |
| 9 | Endpoint | 19 | Vcpu |
| 10 | ReplyToken | 20 | FaultToken |
| 21 | PciFunction | | |

Decoding an attachment does not grant authority. Endpoint authenticates the source handle, checks the expected type,
checks `Transfer` for move or `Duplicate` for copy, proves requested rights and deadline attenuation, reserves receiver
slots and queue budget, and then commits every attachment and the message in one locked transaction. A failure changes
no source/destination slot and publishes no message; a sampled monotonic-clock watermark may still advance.

## Canonical and transactional behavior

The frame size must equal exactly the size reconstructed from the header counts and payload size. All reserved bytes
are zero. The normalized envelope also requires zero in every unused descriptor, unused attachment, and byte beyond
the declared payload. Encoding therefore has one byte representation for each accepted normalized value.

Decoder input must remain readable and stable for the call. Encoder input must remain stable. Input and output ranges
must not overlap; for encoding this applies to the entire writable view capacity, not only the prefix eventually
written. The implementation validates the complete operation before its first output write, so every error leaves the
destination envelope or byte buffer unchanged. Both directions are allocation-free and have fixed loop bounds.

Wire version 1 is an exact contract. A future incompatible representation uses a new version and a separate decoder;
it does not reinterpret reserved fields in version 1.
