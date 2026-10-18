<!-- SPDX-License-Identifier: Apache-2.0 -->

# Object Directory

## Scope

The Object Directory is a user-space Executive service. It is not a kernel
object and the nucleus does not contain a service-name table. The nucleus only
authenticates the Endpoint used for the invocation, transfers capabilities,
and delivers the invoking Endpoint's effective rights and sealed selector as
trusted metadata.

`ObjectDirectoryCore` and `ObjectDirectoryServiceCore` supply the freestanding
core. The native adapter in `Source/User/Directory/DirectoryProgram.cpp`
receives and replies through `SystemCallClient`; the owning root service supplies
its Endpoints, WaitSet and capabilities ([boot service integration](BootPager.md)).

## Namespace nodes

One `ObjectDirectoryCore` represents one namespace node. A name is one bounded
technical component rather than a slash-separated path. System, session, user,
application, and administrative namespaces are constructed by distributing
different starting Directory Endpoint capabilities and by binding child
Directory capabilities where traversal is required.

This keeps namespace selection capability-based:

```text
System Directory capability
  -> Session Directory capability
       -> User Directory capability
       -> Application Directory capability

System Directory capability
  -> Administrative Directory capability
```

User and application directories are siblings. Two nodes may bind the same
name to different endpoints. The core does not accept a user ID, session ID,
principal, parent path, or an `is_admin` field from message payloads. A service
adapter selects the configured node from the trusted sealed selector carried
by Endpoint delivery metadata.

Names contain 1 through 64 lowercase ASCII bytes. The first and last bytes are
alphanumeric; an interior byte may additionally be `.`, `-`, or `_`. Comparison
is byte-exact. There is no locale handling, Unicode normalization, case
folding, wildcard matching, parent traversal, or implicit ancestor fallback.

## Binding table

Each node owns a fixed table of 64 bindings and performs a bounded linear scan.
The core allocates no memory and is serialized by one service dispatch loop.
It contains no kernel lock or reference-counting scheme.

A binding contains:

- its canonical name;
- one Directory-space Endpoint capability handle held in custody;
- the advertised interface identifier and major/minor range;
- a nonzero service-instance generation;
- the maximum rights and lease deadline that may be issued;
- a nonzero revision from the node's global revision sequence.

The interface metadata describes the endpoint; it does not authenticate the
implementation behind it. Component Manager is responsible for compatibility
policy before registration or replacement.

The stored capability has the published rights plus `Duplicate`. `Duplicate`
is needed so a successful resolution reply can copy attenuated authority to
the caller. Stored capabilities carry neither `Inspect` nor `Transfer`: Endpoint
delivery already validates the registered attachment, and the reply copies
directly from the stored handle during the same serialized dispatch turn.

## Operations and ownership

`BindIfAbsent` is create-only. `ReplaceIfVersion` and `RemoveIfVersion` require
the exact name and binding version. A binding version is the pair of a nonzero
Directory generation and a nonzero, directory-global revision. The Supervisor
supplies a new generation for a new Directory instance; the revision advances
once for each committed mutation and never wraps.

Registration and replacement requests copy the provider's target Endpoint.
Delivery commits capability transfer before the service
can evaluate an existing name or stale compare-and-swap token. Moving the
provider's only handle would therefore destroy it even when the semantic
operation is rejected.

On successful bind or replacement, the table adopts the received target
handle. On a failed mutation, the service adapter closes the unadopted received
copy. Successful replacement and removal return the displaced custody handle
to the adapter for closing after the table commit. Cleanup failure does not
roll the visible binding backward.

A successful resolve returns a borrowed view of the stored handle, the
selected highest compatible minor version, and the exact attenuated rights and
deadline. The adapter sends the copy reply before allowing another core
operation. This single-owner ordering makes the lookup and replacement
linearization explicit without hidden temporary capabilities or in-flight
reference counts.

Replacement changes only future resolutions. Capabilities copied before the
cutover remain valid. The Supervisor must drain old work and revoke the old
service's revocation domain separately; the Directory does not revoke it as a
side effect of rebinding.

## Authority and policy boundary

Resolving requires effective `Invoke` authority on the invoked Directory
Endpoint. Binding, replacing, and removing additionally require `Manage`.
These rights and the Directory selector come from trusted delivery metadata,
not from the request payload. The sender resource-domain identifier is useful
for diagnostics and resource accounting but is not a principal or an access
list key.

The core performs only mechanical checks: canonical names, version
intersection, rights/deadline attenuation, fixed capacity, capability custody,
and compare-and-swap replacement. Policy Manager remains responsible for
principals, users, sessions, ACLs, labels, user presence, administrative
approval, and whether name existence may be disclosed.

## Failure and restart model

All validation finishes before the first table mutation. Failed operations
leave the table, input custody, and output values unchanged. A committed
mutation is not rolled back merely because its reply cannot be queued or the
client loses the response. The client re-resolves and compares the returned
version when commit status is unknown.

Capability handles are local to one capability space and boot. They are not
serialized as persistent Directory state. A restart snapshot cannot store raw
selectors and authenticators; replacement processes require transferred capabilities.

Quiescing rejects new work and exposes each remaining custody handle exactly
once for explicit close. The destructor performs no syscall cleanup.

## Unsupported operations

The core does not implement ACL evaluation, authentication, audit logs,
persistence, transaction replay journals, enumeration, watch subscriptions,
replication, general non-Endpoint objects, or cryptographic provenance. It also does not add a syscall, kernel object type,
or firmware dependency.

Object Directory production sources are compiled as a separate Executive
closure for IA-64 and AMD64. They are never appended to or linked into the
nucleus source closure.
