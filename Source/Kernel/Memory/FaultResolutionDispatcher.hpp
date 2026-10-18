// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_RESOLUTION_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_RESOLUTION_DISPATCHER_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/PagerFaultDispatcher.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/ObjectPin.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Memory
{
enum class FaultResolutionDispatcherState : Foundation::UInt8
{
    Fresh,
    ShootdownPending,
    MappingCommitted,
    Completed,
    Cancelled,
    Terminal,
};

/// Describes the strongest native mapping fact known after one operation.
enum class FaultResolutionNativeState : Foundation::UInt8
{
    Unchanged,
    InstalledUnexposed,
    Committed,
    StateUncertain,
};

enum class FaultResolutionDispatcherError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyStarted,
    NotPending,
    AlreadyCompleted,
    Terminal,
    ClaimRejected,
    FaultTokenPinRejected,
    FaultTokenInstanceMismatch,
    CorrelatedStateChanged,
    MappingPreparationRejected,
    AddressSpaceRejected,
    ShootdownRejected,
    MappingCompletionRejected,
    PagerCompletionRejected,
    ClaimCancellationRejected,
    ProtocolViolation,
};

/// All object and protection authority in this request was checked by the
/// capability layer before BeginMap. This dispatcher validates exact storage,
/// registry generations, ranges, and state-machine correlations only; it does
/// not grant or infer pager authority.
struct FaultResolutionMapRequest
{
    FaultToken* FaultTokenStorage;
    AddressSpace* AddressSpaceStorage;
    MemoryObject* MemoryObjectStorage;
    Device::DeviceRegion* DeviceRegionStorage;
    Frame* FrameStorage;
    Object::ObjectId FaultTokenIdentity;
    FaultResolutionClaimToken Claim;
    MemoryObjectId MemoryObjectIdentity;
    Device::DeviceRegionId DeviceRegionIdentity;
    FrameId FrameIdentity;
    FaultFrameMappingRequest Mapping;
    Foundation::UInt8 Reserved[8];
};

struct FaultResolutionDispatcherFailure
{
    Foundation::UInt64 ClaimSequence;
    AddressSpaceFailure AddressSpace;
    TlbShootdownFailure Shootdown;
    PagerFaultDispatcherFailure Pager;
    FaultTokenError Token;
    Object::ObjectPinError Pin;
    FaultFrameMappingError Preparation;
    FaultResolutionDispatcherError Error;
    FaultResolutionDispatcherState State;
    FaultResolutionNativeState NativeState;
    bool Terminal;
    bool ClaimCancelled;
    bool RemoteWorkPublished;
    Foundation::UInt8 Reserved;
};

struct FaultResolutionDispatcherReceipt
{
    AddressSpaceMappingReceipt Mapping;
    TlbShootdownReceipt Shootdown;
    PagerFaultDispatcherReceipt Pager;
    Foundation::UInt64 ClaimSequence;
    FaultResolutionDispatcherState State;
    FaultResolutionNativeState NativeState;
    bool RemoteWorkPublished;
    Foundation::UInt8 Reserved[5];
};

/// Executes one bounded, allocation-free Map resolution in serialized normal context. The exact
/// claim is supplied already active and authorized. BeginMap validates and pins all generations,
/// prepares the mapping, installs it in the native backend, performs the initiating CPU
/// invalidation, and publishes one bounded remote batch. With remote targets it returns a
/// ShootdownPending receipt. The caller drains every published work item and then calls
/// ContinueMap. ContinueMap is bounded and may be polled; an incomplete acknowledgement set remains
/// ShootdownPending. Once the supplied claim is authenticated, every subsequent failure before a
/// possible native commit cancels that exact claim. A foreign, stale, or replayed claim is rejected
/// without changing any claim generation. Once native state may have changed, the claim is retained
/// and failures explicitly report a terminal or retryable post-commit state. The faulting Thread is
/// woken only by PagerFaultDispatcher after CompleteFaultMapping succeeds. Lifetime and
/// concurrency: - every referenced object and transport outlives this dispatcher; - the caller
/// serializes this one-shot dispatcher and the supplied claim; - calls are forbidden from interrupt
/// context and same-CPU reentry; - every successfully published remote work item is joined before
/// this dispatcher or its AddressSpace can be destroyed.
class FaultResolutionDispatcher final
{
public:
    constexpr FaultResolutionDispatcher(Object::ObjectRegistry& objectRegistry,
                                        TlbShootdownDispatcher& shootdownDispatcher,
                                        Hal::Memory::ITlbShootdownTransport& transport,
                                        PagerFaultDispatcher& pagerDispatcher) noexcept
        : m_objectRegistry{objectRegistry}, m_shootdownDispatcher{shootdownDispatcher}, m_transport{transport},
          m_pagerDispatcher{pagerDispatcher}
    {
    }

    ~FaultResolutionDispatcher() noexcept = default;

    FaultResolutionDispatcher(const FaultResolutionDispatcher&) = delete;
    FaultResolutionDispatcher& operator=(const FaultResolutionDispatcher&) = delete;
    FaultResolutionDispatcher(FaultResolutionDispatcher&&) = delete;
    FaultResolutionDispatcher& operator=(FaultResolutionDispatcher&&) = delete;

    [[nodiscard]] Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
    BeginMap(const FaultResolutionMapRequest& request) noexcept;

    [[nodiscard]] Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
    ContinueMap() noexcept;

    /// Returns a Completed or Cancelled dispatcher, or a Fresh one, to Fresh
    /// for the next resolution, releasing the token and mapping pins. Any
    /// other state retains its evidence and reports AlreadyStarted.
    [[nodiscard]] FaultResolutionDispatcherError Reset() noexcept;

    [[nodiscard]] constexpr FaultResolutionDispatcherState GetState() const noexcept
    {
        return m_state;
    }

private:
    [[nodiscard]] static FaultResolutionDispatcherFailure MakeFailure(FaultResolutionDispatcherError error,
                                                                      FaultResolutionDispatcherState state,
                                                                      FaultResolutionNativeState nativeState,
                                                                      Foundation::UInt64 claimSequence = 0U) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool ValidateRequest(const FaultResolutionMapRequest& request,
                                                              FaultResolutionDispatcherFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool AuthenticateClaim(const FaultResolutionMapRequest& request,
                                                                FaultResolutionDispatcherFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ValidateConfiguration(const FaultResolutionMapRequest& request,
                          FaultResolutionDispatcherFailure& failure) const noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool PinFaultToken(const FaultResolutionMapRequest& request,
                                                            FaultResolutionDispatcherFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ValidateAddressSpaceCorrelation(FaultResolutionDispatcherFailure& failure) const noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ValidatePagerCorrelation(const FaultResolutionMapRequest& request,
                             FaultResolutionDispatcherFailure& failure) const noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool PrepareMapping(const FaultResolutionMapRequest& request,
                                                             FaultResolutionDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool CancelBeforeNative(FaultResolutionDispatcherFailure& failure, bool ownerIsTerminal) noexcept;
    [[nodiscard]] Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
    CompleteMappingAndPager() noexcept;
    [[nodiscard]] Foundation::Result<FaultResolutionDispatcherReceipt, FaultResolutionDispatcherFailure>
    CompletePager() noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool RequestPagerCompletion(PagerFaultDispatcherFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool PagerCompletionFailureIsTerminal() const noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool PagerCompletionIsValid() const noexcept;
    [[nodiscard]] FaultResolutionDispatcherReceipt MakeReceipt() const noexcept;
    void EnterTerminal(FaultResolutionNativeState nativeState) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    TlbShootdownDispatcher& m_shootdownDispatcher;
    Hal::Memory::ITlbShootdownTransport& m_transport;
    PagerFaultDispatcher& m_pagerDispatcher;
    Object::ObjectPin m_faultTokenPin;
    FaultFrameMappingPreparation m_preparation;
    FaultToken* m_faultToken{nullptr};
    AddressSpace* m_addressSpace{nullptr};
    FaultResolutionClaimToken m_claim{};
    Scheduler::ThreadId m_threadIdentity{};
    AddressSpaceId m_addressSpaceIdentity{};
    AddressSpaceMappingToken m_mappingToken{};
    AddressSpaceMappingReceipt m_mappingReceipt{};
    TlbShootdownReceipt m_shootdownReceipt{};
    PagerFaultDispatcherReceipt m_pagerReceipt{};
    Foundation::UInt64 m_claimSequence{0U};
    FaultResolutionDispatcherState m_state{FaultResolutionDispatcherState::Fresh};
    FaultResolutionNativeState m_nativeState{FaultResolutionNativeState::Unchanged};
    bool m_remoteWorkPublished{false};
};

static_assert(sizeof(FaultResolutionDispatcherState) == 1U);
static_assert(sizeof(FaultResolutionNativeState) == 1U);
static_assert(sizeof(FaultResolutionDispatcherError) == 1U);
static_assert(sizeof(FaultResolutionMapRequest) == 160U);
static_assert(alignof(FaultResolutionMapRequest) == 8U);
static_assert(sizeof(FaultResolutionDispatcherFailure) == 64U);
static_assert(alignof(FaultResolutionDispatcherFailure) == 8U);
static_assert(sizeof(FaultResolutionDispatcherReceipt) == 144U);
static_assert(alignof(FaultResolutionDispatcherReceipt) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FAULT_RESOLUTION_DISPATCHER_HPP_INCLUDED
