// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_OBJECT_CAPABILITY_SPACE_HPP_INCLUDED
#define ASTERIA_KERNEL_OBJECT_CAPABILITY_SPACE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTransfer.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Time/IMonotonicClock.hpp"
#include "Platform/Synchronization/SpinLock.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Kernel::Ipc
{
class Endpoint;
}

namespace Asteria::Kernel::Object
{
class RootCapabilityIssuer;

class CapabilityLease final
{
public:
    constexpr CapabilityLease() noexcept = default;

    /// Releases one reference using a single wait-free atomic operation.
    /// Interrupt safety:
    /// - Safe with respect to locking; object operations still define their own ISR contract.
    ~CapabilityLease() noexcept;

    CapabilityLease(const CapabilityLease&) = delete;
    CapabilityLease& operator=(const CapabilityLease&) = delete;
    CapabilityLease(CapabilityLease&& other) noexcept;
    CapabilityLease& operator=(CapabilityLease&&) = delete;

    /// Returns whether this lease pins an object against final destruction.
    [[nodiscard]] bool IsValid() const noexcept;

    /// Returns the immutable capability metadata captured by Lookup.
    /// Preconditions:
    /// - IsValid is true.
    [[nodiscard]] const CapabilityDescriptor& GetDescriptor() const noexcept;

    /// Returns the pinned object instance.
    /// Preconditions:
    /// - IsValid is true.
    [[nodiscard]] IKernelObject& GetObject() const noexcept;

private:
    friend class CapabilitySpace;

    CapabilityLease(ObjectRegistry& objectRegistry, IKernelObject& object,
                    const CapabilityDescriptor& descriptor) noexcept;

    ObjectRegistry* m_objectRegistry{nullptr};
    IKernelObject* m_object{nullptr};
    CapabilityDescriptor m_descriptor{};
};

class [[nodiscard]] CapabilityLookupResult final
{
public:
    ~CapabilityLookupResult() noexcept = default;

    CapabilityLookupResult(const CapabilityLookupResult&) = delete;
    CapabilityLookupResult& operator=(const CapabilityLookupResult&) = delete;
    CapabilityLookupResult(CapabilityLookupResult&& other) noexcept;
    CapabilityLookupResult& operator=(CapabilityLookupResult&&) = delete;

    [[nodiscard]] bool HasValue() const noexcept;

    /// Preconditions:
    /// - HasValue is true.
    [[nodiscard]] const CapabilityLease& GetValue() const noexcept;
    [[nodiscard]] CapabilityError GetError() const noexcept;

private:
    friend class CapabilitySpace;

    [[nodiscard]] static CapabilityLookupResult Success(CapabilityLease&& lease) noexcept;
    [[nodiscard]] static CapabilityLookupResult Failure(CapabilityError error) noexcept;

    CapabilityLookupResult(CapabilityLease&& lease, CapabilityError error, bool hasValue) noexcept;

    CapabilityLease m_lease;
    CapabilityError m_error;
    bool m_hasValue;
};

/// The revocation domain a derived capability is minted in.
enum class DerivedRevocationDomain : Foundation::UInt8
{
    /// The source's domain; revoking it also revokes the source.
    Source,
    /// A new child of the source's domain, revocable without the source.
    Child,
};

class CapabilitySpace final
{
public:
    static constexpr Foundation::Size MaximumSlots{256U};

    /// Execution context for all methods that may acquire the shared registry lock:
    /// - The caller prevents same-CPU preemption or reentry by code using that lock.

    /// Creates an empty capability space bound to the kernel object state and clock.
    /// Preconditions:
    /// - Both registries use the same synchronization object.
    /// - The revocation registry successfully acquired its boot-unique identifier.
    /// - All three collaborators and their synchronization object outlive this space and its
    /// outstanding lookup leases.
    /// - The validation keys come from the boot entropy source and are not both zero.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    /// Blocking:
    /// - May spin while another CPU allocates a boot-unique capability-space identifier.
    /// Allocation:
    /// - Does not allocate.
    CapabilitySpace(ObjectRegistry& objectRegistry, RevocationRegistry& revocationRegistry,
                    Time::IMonotonicClock& clock, Foundation::UInt64 validationKey0,
                    Foundation::UInt64 validationKey1) noexcept;

    CapabilitySpace(const CapabilitySpace&) = delete;
    CapabilitySpace& operator=(const CapabilitySpace&) = delete;
    CapabilitySpace(CapabilitySpace&&) = delete;
    CapabilitySpace& operator=(CapabilitySpace&&) = delete;

    /// Resolves and authorizes a handle, returning a dispatch-scoped object-pinning lease.
    /// Closing, revoking, or expiring the capability prevents later lookups but does not cancel
    /// an operation that already owns a lease. Callers must not retain leases beyond one dispatch.
    /// An operation whose effect is published later under the shared lock, such as an Endpoint
    /// send or receive, re-authorizes the handle in that critical section.
    /// Blocking:
    /// - May spin while another CPU holds the shared registry lock.
    /// Allocation:
    /// - Does not allocate.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    // Current Interrupt and port-I/O authority. Memory-mapped framebuffer authority does not
    // restrict placement.
    [[nodiscard]] bool HasProcessorDeviceAuthority() noexcept;

    [[nodiscard]] CapabilityLookupResult Lookup(CapabilityHandle handle, ObjectType expectedType,
                                                CapabilityOperation operation) noexcept;

    /// Creates an attenuated capability when source carries Derive. The result either has strictly
    /// fewer rights or, with DerivedRevocationDomain::Child, a new child of the source's revocation
    /// domain, which is the only way it may carry Revoke: Revoke over the source's own domain would
    /// let the result revoke its source. A child domain is created only after every other check and
    /// a free slot are confirmed, so failure creates none.
    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Derive(CapabilityHandle source, CapabilityRights requestedRights, MonotonicTime requestedDeadline,
           DerivedRevocationDomain revocationDomain = DerivedRevocationDomain::Source) noexcept;

    /// Creates an equal or attenuated capability when source carries Duplicate.
    /// The result shares the source's revocation domain and so may not carry
    /// Revoke.
    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Duplicate(CapabilityHandle source, CapabilityRights requestedRights, MonotonicTime requestedDeadline) noexcept;

    /// Returns an Inspect-authorized immutable descriptor snapshot. The
    /// descriptor conveys no authority and is valid only as observed metadata.
    [[nodiscard]] Foundation::Result<CapabilityDescriptor, CapabilityError> Query(CapabilityHandle handle) noexcept;

    /// Revokes the exact domain named by a capability carrying Revoke. The
    /// authorization and epoch transition share one lock linearization point.
    [[nodiscard]] CapabilityError Revoke(CapabilityHandle handle) noexcept;

    /// Atomically moves or copies a bounded batch into another capability space. Every request is
    /// authenticated, current, unexpired, type-matched, and attenuated before any capability slot
    /// is changed. Move requires Transfer; copy requires Duplicate. Failure leaves every source and
    /// destination slot unchanged. A zero-count batch is a successful no-op that inspects neither
    /// space, its lock, nor the clock. A failed nonempty call may still advance either space's
    /// monotonic-clock watermark. Preconditions: - Both spaces and the request view remain alive
    /// and stable for the call. - The caller prevents same-CPU preemption or reentry by code using
    /// the shared lock. Blocking: - May spin while another CPU holds the shared registry lock.
    /// Allocation: - Does not allocate. Interrupt safety: - Must not be called from an ISR.
    [[nodiscard]] Foundation::Result<CapabilityTransferBatch, CapabilityError>
    TransferBatchTo(CapabilitySpace& destination, CapabilityTransferRequestView requests) noexcept;

    /// Invalidates handle and eventually retires its slot if its generation is exhausted.
    [[nodiscard]] CapabilityError Close(CapabilityHandle handle) noexcept;

    [[nodiscard]] Foundation::Size GetActiveCount() const noexcept;
    /// The capabilities a prepared send holds (moved out but not yet
    /// published); a space being torn down must have none.
    [[nodiscard]] Foundation::Size GetHeldCount() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] CapabilitySpaceIdentifier GetIdentifier() const noexcept;
    [[nodiscard]] bool IsFromRegistry(const ObjectRegistry& objectRegistry) const noexcept;
    [[nodiscard]] bool IsFromRevocationRegistry(const RevocationRegistry& revocationRegistry) const noexcept;

private:
    friend class RootCapabilityIssuer;
    friend class Asteria::Kernel::Ipc::Endpoint;

    /// How a transfer commits a moved source. Release frees it at once; Hold
    /// keeps its slot, generation, and descriptor but makes it unusable until
    /// ReleaseHeldLocked frees it or RestoreHeldLocked makes the same handle
    /// usable again, so a prepared send removes a moved source only when it
    /// publishes.
    enum class MovedSourceCommit : Foundation::UInt8
    {
        Release,
        Hold,
    };

    /// A held slot stays occupied and counted as active; no handle resolves
    /// to it until it is released or restored.
    [[nodiscard]] CapabilityLookupResult PlacementAuthority(Foundation::Size index) noexcept;
    struct SlotEntry
    {
        bool IsOccupied;
        bool IsRetired;
        bool IsHeld;
        Foundation::UInt64 Generation;
        Foundation::UInt32 RevocationEpoch;
        CapabilityDescriptor Descriptor;
    };

    struct CapabilityTransferPlan
    {
        CapabilityDescriptor Descriptors[CapabilityTransferBatch::MaximumTransfers];
        Foundation::Size SourceIndices[CapabilityTransferBatch::MaximumTransfers];
        Foundation::Size DestinationIndices[CapabilityTransferBatch::MaximumTransfers];
        Foundation::UInt32 RevocationEpochs[CapabilityTransferBatch::MaximumTransfers];
        CapabilityTransferDisposition Dispositions[CapabilityTransferBatch::MaximumTransfers];
    };

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintFromRootIssuer(const ObjectRegistry& issuerRegistry, ObjectId object, CapabilityRights rights,
                       RevocationDomainId revocationDomain, MonotonicTime leaseDeadline,
                       Foundation::UInt64 sealedObjectSelector) noexcept;

    /// Mints object under authority, a capability in this space, in one
    /// critical section that authorizes authority as Lookup would (usable
    /// with operation's right, of authorityType, naming exactly
    /// authorityObject) and inserts the new capability in authority's
    /// revocation domain and epoch, with authority's lease deadline and
    /// sealedObjectSelector. A Revoke, Close, or expiry of authority ordered
    /// before the mint therefore fails it.
    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    MintUnderAuthorityFromRootIssuer(const ObjectRegistry& issuerRegistry, ObjectId object, CapabilityRights rights,
                                     CapabilityHandle authority, ObjectType authorityType,
                                     CapabilityOperation operation, const IKernelObject& authorityObject,
                                     Foundation::UInt64 sealedObjectSelector = 0U) noexcept;

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    InsertDescriptorLocked(const CapabilityDescriptor& descriptor, Foundation::UInt32 revocationEpoch) noexcept;

    /// Whether both spaces are ready and share the object registry, revocation
    /// registry, clock, and lock, as every transfer and invocation requires.
    /// Reads only immutable configuration, so it is safe without the lock.
    [[nodiscard]] bool SharesTransferDomain(const CapabilitySpace& destination) const noexcept;

    /// Samples the shared clock once for an operation on this space and
    /// destination (possibly the same space). A sample that is infinite or
    /// behind either watermark fails with InvalidConfiguration; otherwise both
    /// watermarks advance to it.
    /// Preconditions:
    /// - SharesTransferDomain(destination) and the shared lock is held.
    [[nodiscard]] Foundation::Result<MonotonicTime, CapabilityError>
    SampleTransferTimeLocked(CapabilitySpace& destination) noexcept;

    /// Transfers a batch at a clock sample taken by SampleTransferTimeLocked
    /// for this operation. A zero-count batch succeeds without inspecting
    /// either space.
    [[nodiscard]] Foundation::Result<CapabilityTransferBatch, CapabilityError>
    TransferBatchToLocked(CapabilitySpace& destination, CapabilityTransferRequestView requests, MonotonicTime now,
                          MovedSourceCommit movedSourceCommit) noexcept;

    /// Authorizes one kernel invocation of expectedObject through handle at
    /// now, with the checks and error precedence of Lookup: the capability
    /// must be usable with the operation's right, of expectedType, and name
    /// exactly expectedObject (InvalidArgument otherwise). Takes no reference.
    [[nodiscard]] Foundation::Result<CapabilityDescriptor, CapabilityError>
    AuthorizeInvocationLocked(CapabilityHandle handle, ObjectType expectedType, CapabilityOperation operation,
                              const IKernelObject& expectedObject, MonotonicTime now) const noexcept;

    /// Resolves an authentic handle whose slot is held by a prepared transfer.
    [[nodiscard]] Foundation::Result<Foundation::Size, CapabilityError>
    ResolveHeldHandleLocked(CapabilityHandle handle) const noexcept;

    /// Frees a held slot as a committed move (generation advance or retire).
    void ReleaseHeldLocked(Foundation::Size index) noexcept;

    /// Makes a held slot usable again under its unchanged handle.
    void RestoreHeldLocked(Foundation::Size index) noexcept;

    [[nodiscard]] CapabilityError CloseLocked(CapabilityHandle handle) noexcept;
    void CloseIndexLocked(Foundation::Size index) noexcept;

    [[nodiscard]] CapabilityError PrepareTransferPlanLocked(CapabilitySpace& destination,
                                                            CapabilityTransferRequestView requests, MonotonicTime now,
                                                            MovedSourceCommit movedSourceCommit,
                                                            CapabilityTransferPlan& plan) noexcept;

    [[nodiscard]] Foundation::Result<CapabilityTransferBatch, CapabilityError>
    CommitTransferPlanLocked(CapabilitySpace& destination, const CapabilityTransferPlan& plan, Foundation::Size count,
                             MovedSourceCommit movedSourceCommit) noexcept;

    [[nodiscard]] Foundation::Result<CapabilityDescriptor, CapabilityError>
    GetUsableDescriptorLocked(CapabilityHandle handle, CapabilityRights requiredRights,
                              MonotonicTime now) const noexcept;

    [[nodiscard]] Foundation::Result<Foundation::Size, CapabilityError>
    ResolveHandleLocked(CapabilityHandle handle) const noexcept;

    [[nodiscard]] Foundation::Result<MonotonicTime, CapabilityError> GetCurrentTimeLocked() noexcept;

    [[nodiscard]] CapabilityHandle EncodeHandleLocked(Foundation::Size index,
                                                      Foundation::UInt64 generation) const noexcept;

    [[nodiscard]] Foundation::UInt64 CalculateAuthenticator(Foundation::UInt64 selector) const noexcept;

    [[nodiscard]] static Foundation::Result<CapabilityRights, CapabilityError>
    GetRequiredRights(CapabilityOperation operation) noexcept;

    [[nodiscard]] static bool IsExpired(MonotonicTime deadline, MonotonicTime now) noexcept;

    [[nodiscard]] bool HasFreeSlotLocked() const noexcept;

    ObjectRegistry& m_objectRegistry;
    RevocationRegistry& m_revocationRegistry;
    Time::IMonotonicClock& m_clock;
    Platform::Synchronization::SpinLock& m_synchronization;
    CapabilitySpaceIdentifier m_spaceIdentifier;
    Foundation::UInt64 m_validationKey0;
    Foundation::UInt64 m_validationKey1;
    bool m_isConfigurationValid;
    // A regression is local to one processor.
    MonotonicTime m_lastObservedTime[Scheduler::MaximumProcessorCount]{};
    [[nodiscard]] static Foundation::Size ClockProcessor() noexcept
    {
        const auto processor = Platform::Synchronization::CurrentProcessor();
        return processor.IsValid() ? processor.GetValue() : 0U;
    }
    SlotEntry m_entries[MaximumSlots]{};
    Foundation::Size m_activeCount{0U};
};
} // Namespace Asteria::Kernel::Object

#endif // ASTERIA_KERNEL_OBJECT_CAPABILITY_SPACE_HPP_INCLUDED
