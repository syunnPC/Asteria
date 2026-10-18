// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"

#include "Support/TestContext.hpp"
#include <cstdio>
#include <new>

using Asteria::TestSupport::TestContext;

extern "C" Asteria::Foundation::UInt64 AsteriaIa64GetStartKernelContextAddress() noexcept;

namespace Asteria::Test
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Platform::Ia64::Scheduler::Ia64KernelContext;
using Platform::Ia64::Scheduler::Ia64KernelContextError;
using Platform::Ia64::Scheduler::Ia64KernelContextResolvedEntry;
using Platform::Ia64::Scheduler::Ia64KernelContextSnapshot;
using Platform::Ia64::Scheduler::Ia64KernelContextState;
using Platform::Ia64::Scheduler::Ia64PageFaultFrameBindingError;
using Platform::Ia64::Scheduler::Ia64ExternalFrameIdleContinuationError;
using Platform::Ia64::Scheduler::Ia64ExternalFrameIdleContinuationSnapshot;
using Platform::Ia64::Scheduler::Ia64KernelRegisterState;
using Platform::Ia64::Scheduler::Ia64KernelDefaultFloatingPointStatus;
using Platform::Ia64::Scheduler::Ia64SynchronousContinuationSwitchError;
using Platform::Ia64::Scheduler::MaximumIa64KernelMemoryStackSizeBytes;
using Platform::Ia64::Scheduler::MaximumIa64KernelRseBackingStoreSizeBytes;
using Platform::Ia64::Scheduler::WritableIa64KernelStorageView;
using Platform::Ia64::Scheduler::SwitchIa64SynchronousContinuation;
using Platform::Ia64::Interrupt::AdvanceIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::BeginIa64ExternalPreemptionCapture;
using Platform::Ia64::Interrupt::CommitIa64ExternalPreemptionResume;
using Platform::Ia64::Interrupt::CompleteIa64ExternalPreemptionTimerAcknowledgement;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureRequest;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureStage;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrame;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrameError;
using Platform::Ia64::Interrupt::Ia64ExternalPreemptionTimerEvidence;
using Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameState;
using Platform::Ia64::Interrupt::ModelReleaseIa64ExternalPreemptionFrameForRfi;
using Platform::Ia64::Interrupt::PublishIa64ExternalPreemptionCapture;

namespace
{
constexpr UInt8 AddressMostSignificantBit{53U};
constexpr Size StorageBytes{16U * 1024U};
constexpr Size OversizedMemoryStackBytes{MaximumIa64KernelMemoryStackSizeBytes + 4096U};
constexpr Size OversizedRseBackingStoreBytes{MaximumIa64KernelRseBackingStoreSizeBytes + 4096U};
constexpr UInt64 RemoteVirtualStartA{0x0001'0000'0000'0000U};
constexpr UInt64 RemoteVirtualStartB{0x0001'0000'0001'0000U};

alignas(4096) UInt8 g_memoryStack[StorageBytes]{};
alignas(4096) UInt8 g_rseBackingStore[StorageBytes]{};
alignas(4096) UInt8 g_alternateMemoryStack[StorageBytes]{};
alignas(4096) UInt8 g_alternateRseBackingStore[StorageBytes]{};
alignas(4096) UInt8 g_externalIdleMemoryStack[StorageBytes]{};
alignas(4096) UInt8 g_externalIdleRseBackingStore[2U * StorageBytes]{};
alignas(4096) UInt8 g_aliasStorage[StorageBytes]{};
alignas(4096) UInt8 g_oversizedMemoryStack[OversizedMemoryStackBytes]{};
alignas(4096) UInt8 g_oversizedRseBackingStore[OversizedRseBackingStoreBytes]{};

Size g_switchCount{0U};
Ia64KernelRegisterState* g_currentRegisters{nullptr};
const Ia64KernelRegisterState* g_nextRegisters{nullptr};

void Entry(void*) noexcept
{
}

[[nodiscard]] WritableIa64KernelStorageView Storage(UInt8* const data, const UInt64 virtualStart = 0U,
                                                    const UInt64 sizeBytes = StorageBytes) noexcept
{
    const UInt64 dataAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(data));
    return WritableIa64KernelStorageView{
        .Data = data,
        .SizeBytes = sizeBytes,
        .VirtualStart = virtualStart == 0U ? dataAddress : virtualStart,
    };
}

[[nodiscard]] bool IsFreshSnapshot(const Ia64KernelContextSnapshot& snapshot) noexcept
{
    bool reservedIsZero{true};
    for (Size index{0U}; index < 6U; ++index)
    {
        reservedIsZero = reservedIsZero && (snapshot.Reserved[index] == 0U);
    }

    return (snapshot.MemoryStackVirtualStart == 0U) && (snapshot.MemoryStackSizeBytes == 0U) &&
           (snapshot.RseBackingStoreVirtualStart == 0U) && (snapshot.RseBackingStoreSizeBytes == 0U) &&
           (snapshot.EntryDescriptorAddress == 0U) && (snapshot.EntryPointAddress == 0U) &&
           (snapshot.ArgumentAddress == 0U) && (snapshot.State == Ia64KernelContextState::Fresh) &&
           (snapshot.ImplementedVirtualAddressMostSignificantBit == 0U) && reservedIsZero;
}

void FillBytes(UInt8* const storage, const Size size, const UInt8 value) noexcept
{
    for (Size index{0U}; index < size; ++index)
    {
        storage[index] = value;
    }
}

[[nodiscard]] bool BytesHaveValue(const UInt8* const storage, const Size size, const UInt8 value) noexcept
{
    for (Size index{0U}; index < size; ++index)
    {
        if (storage[index] != value)
        {
            return false;
        }
    }
    return true;
}

void CopyBytes(const void* const source, UInt8* const destination, const Size size) noexcept
{
    const auto* const sourceBytes = static_cast<const UInt8*>(source);
    for (Size index{0U}; index < size; ++index)
    {
        destination[index] = sourceBytes[index];
    }
}

[[nodiscard]] bool BytesAreEqual(const void* const value, const UInt8* const expected, const Size size) noexcept
{
    const auto* const valueBytes = static_cast<const UInt8*>(value);
    for (Size index{0U}; index < size; ++index)
    {
        if (valueBytes[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsZeroRegister(const Platform::Ia64::Scheduler::Ia64FloatingRegisterState& value) noexcept
{
    return (value.Low == 0U) && (value.High == 0U);
}

[[nodiscard]] bool PreparedRegistersAreExact(const Ia64KernelRegisterState& registers,
                                             const UInt64 expectedStackPointer, const UInt64 expectedRseStart,
                                             const UInt64 expectedEntry, const UInt64 expectedGlobalPointer,
                                             const UInt64 expectedArgument,
                                             const UInt64 expectedInstructionPointer) noexcept
{
    bool floatingRegistersAreZero{true};
    for (Size index{0U}; index < 20U; ++index)
    {
        floatingRegistersAreZero = floatingRegistersAreZero && IsZeroRegister(registers.FloatingRegisters[index]);
    }

    return (registers.R4 == expectedEntry) && (registers.R5 == expectedGlobalPointer) &&
           (registers.R6 == expectedArgument) && (registers.R7 == 0U) && (registers.GlobalPointer == 0U) &&
           (registers.StackPointer == expectedStackPointer) && (registers.ThreadPointer == 0U) &&
           (registers.InstructionPointer == expectedInstructionPointer) && (registers.PreviousFunctionState == 0U) &&
           (registers.RseConfiguration == 0U) && (registers.RseBackingStorePointer == expectedRseStart) &&
           (registers.RseNatCollection == 0U) && (registers.UserNatCollection == 0U) &&
           (registers.PreservedGeneralNatCollection == 0U) && (registers.LoopCounter == 0U) &&
           (registers.FloatingPointStatus == Ia64KernelDefaultFloatingPointStatus) && (registers.Predicates == 0U) &&
           (registers.B1 == 0U) && (registers.B2 == 0U) && (registers.B3 == 0U) && (registers.B4 == 0U) &&
           (registers.B5 == 0U) && floatingRegistersAreZero;
}

void ResetAdapter() noexcept
{
    g_switchCount = 0U;
    g_currentRegisters = nullptr;
    g_nextRegisters = nullptr;
}

[[nodiscard]] bool RetainExternalFrame(Ia64ExternalPreemptionFrame& frame, UInt64& token) noexcept
{
    constexpr UInt64 deadline{100U};
    constexpr Ia64ExternalPreemptionCaptureStage stages[]{
        Ia64ExternalPreemptionCaptureStage::InterruptionState, Ia64ExternalPreemptionCaptureStage::IntegerState,
        Ia64ExternalPreemptionCaptureStage::ApplicationState,  Ia64ExternalPreemptionCaptureStage::FloatingState,
        Ia64ExternalPreemptionCaptureStage::RseTransition,
    };
    if (BeginIa64ExternalPreemptionCapture(frame,
                                           Ia64ExternalPreemptionCaptureRequest{
                                               .ProcessorIndex = 0U,
                                               .TimerVector = Ia64ExternalInterruptTimerVector,
                                               .ProgrammedDeadlineTicks = deadline,
                                               .Reserved = 0U,
                                           },
                                           token) != Ia64ExternalPreemptionFrameError::None)
    {
        return false;
    }
    for (const auto stage : stages)
    {
        if (AdvanceIa64ExternalPreemptionCapture(frame, token, stage) != Ia64ExternalPreemptionFrameError::None)
        {
            return false;
        }
    }
    return (CompleteIa64ExternalPreemptionTimerAcknowledgement(
                frame, token,
                Ia64ExternalPreemptionTimerEvidence{
                    .ClaimedVector = Ia64ExternalInterruptTimerVector,
                    .ObservedTicks = deadline,
                    .MaskedTimerVector = Ia64ExternalInterruptMaskedTimerVector,
                    .EoiCount = 1U,
                }) == Ia64ExternalPreemptionFrameError::None) &&
           (PublishIa64ExternalPreemptionCapture(frame, token) == Ia64ExternalPreemptionFrameError::None);
}

[[nodiscard]] bool ReleaseExternalFrame(Ia64ExternalPreemptionFrame& frame, const UInt64 token) noexcept
{
    return (CommitIa64ExternalPreemptionResume(frame, token) == Ia64ExternalPreemptionFrameError::None) &&
           (ModelReleaseIa64ExternalPreemptionFrameForRfi(frame, token) == Ia64ExternalPreemptionFrameError::None);
}

void TestValidation(TestContext& runner) noexcept
{
    constexpr UInt8 MemoryStackSentinel{0xA5U};
    constexpr UInt8 RseBackingStoreSentinel{0x5AU};
    constexpr UInt8 OversizedMemoryStackSentinel{0x3CU};
    constexpr UInt8 OversizedRseBackingStoreSentinel{0xC3U};
    FillBytes(g_memoryStack, StorageBytes, MemoryStackSentinel);
    FillBytes(g_rseBackingStore, StorageBytes, RseBackingStoreSentinel);
    FillBytes(g_oversizedMemoryStack, OversizedMemoryStackBytes, OversizedMemoryStackSentinel);
    FillBytes(g_oversizedRseBackingStore, OversizedRseBackingStoreBytes, OversizedRseBackingStoreSentinel);

    const auto memoryStack = Storage(g_memoryStack);
    const auto rseBackingStore = Storage(g_rseBackingStore);

    Ia64KernelContext fresh;
    UInt8 freshBytes[sizeof(Ia64KernelContext)]{};
    CopyBytes(&fresh, freshBytes, sizeof(fresh));
    const auto failureIsAtomic =
        [&fresh, &freshBytes](const Ia64KernelContextError actual, const Ia64KernelContextError expected) noexcept
    {
        return (actual == expected) && IsFreshSnapshot(fresh.GetSnapshot()) &&
               BytesAreEqual(&fresh, freshBytes, sizeof(fresh));
    };
    ASTERIA_EXPECT(runner, "a fresh context has no published storage or register state", !fresh.IsPrepared(),
                   IsFreshSnapshot(fresh.GetSnapshot()));

    ASTERIA_EXPECT(runner, "an address profile below the PAL-supported range is rejected",
                   failureIsAtomic(fresh.Prepare(49U, memoryStack, rseBackingStore, &Entry, nullptr),
                                   Ia64KernelContextError::InvalidAddressProfile));
    ASTERIA_EXPECT(runner, "an address profile above the architected region offset is rejected",
                   failureIsAtomic(fresh.Prepare(61U, memoryStack, rseBackingStore, &Entry, nullptr),
                                   Ia64KernelContextError::InvalidAddressProfile));

    auto invalidMemoryStack = memoryStack;
    invalidMemoryStack.Data = nullptr;
    ASTERIA_EXPECT(
        runner, "a null memory-stack alias is rejected without publication",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, invalidMemoryStack, rseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidMemoryStack));

    invalidMemoryStack = memoryStack;
    invalidMemoryStack.SizeBytes = StorageBytes - 4096U;
    ASTERIA_EXPECT(
        runner, "a memory stack shorter than sixteen KiB is rejected",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, invalidMemoryStack, rseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidMemoryStack));

    invalidMemoryStack = Storage(g_oversizedMemoryStack, 0U, OversizedMemoryStackBytes);
    ASTERIA_EXPECT(
        runner, "a memory stack larger than sixty-four KiB is rejected atomically",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, invalidMemoryStack, rseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidMemoryStack),
        BytesHaveValue(g_oversizedMemoryStack, OversizedMemoryStackBytes, OversizedMemoryStackSentinel));

    invalidMemoryStack = memoryStack;
    invalidMemoryStack.SizeBytes = StorageBytes + 1U;
    ASTERIA_EXPECT(
        runner, "a non-page-sized memory-stack extent is rejected",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, invalidMemoryStack, rseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidMemoryStack));

    invalidMemoryStack = memoryStack;
    invalidMemoryStack.VirtualStart += 1U;
    ASTERIA_EXPECT(
        runner, "an unaligned memory-stack virtual range is rejected",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, invalidMemoryStack, rseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidMemoryStack));

    invalidMemoryStack = memoryStack;
    invalidMemoryStack.VirtualStart = Foundation::UInt64{1U} << 54U;
    ASTERIA_EXPECT(
        runner, "a nonimplemented IA-64 memory-stack address is rejected",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, invalidMemoryStack, rseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidMemoryStack));

    auto invalidRseBackingStore = rseBackingStore;
    invalidRseBackingStore.Data = invalidRseBackingStore.Data + 1U;
    ASTERIA_EXPECT(
        runner, "an unaligned RSE backing-store alias is rejected independently",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, memoryStack, invalidRseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidRseBackingStore));

    invalidRseBackingStore = rseBackingStore;
    invalidRseBackingStore.SizeBytes = StorageBytes - 4096U;
    ASTERIA_EXPECT(
        runner, "an RSE backing store shorter than sixteen KiB is rejected",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, memoryStack, invalidRseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidRseBackingStore));

    invalidRseBackingStore = Storage(g_oversizedRseBackingStore, 0U, OversizedRseBackingStoreBytes);
    ASTERIA_EXPECT(
        runner, "an RSE backing store larger than sixty-four KiB is rejected atomically",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, memoryStack, invalidRseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidRseBackingStore),
        BytesHaveValue(g_oversizedRseBackingStore, OversizedRseBackingStoreBytes, OversizedRseBackingStoreSentinel));

    invalidRseBackingStore = rseBackingStore;
    invalidRseBackingStore.SizeBytes = StorageBytes + 1U;
    ASTERIA_EXPECT(
        runner, "a non-page-sized RSE backing-store extent is rejected",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, memoryStack, invalidRseBackingStore, &Entry, nullptr),
                        Ia64KernelContextError::InvalidRseBackingStore));

    const auto dataAliasedMemoryStack = Storage(g_aliasStorage, RemoteVirtualStartA);
    const auto dataAliasedRseBackingStore = Storage(g_aliasStorage, RemoteVirtualStartB);
    ASTERIA_EXPECT(runner, "distinct virtual ranges cannot share physical storage bytes",
                   failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, dataAliasedMemoryStack,
                                                 dataAliasedRseBackingStore, &Entry, nullptr),
                                   Ia64KernelContextError::StorageRangesOverlap));

    const auto virtuallyAliasedMemoryStack = Storage(g_memoryStack, RemoteVirtualStartA);
    const auto virtuallyAliasedRseBackingStore = Storage(g_rseBackingStore, RemoteVirtualStartA);
    ASTERIA_EXPECT(runner, "distinct storage bytes cannot share one virtual range",
                   failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, virtuallyAliasedMemoryStack,
                                                 virtuallyAliasedRseBackingStore, &Entry, nullptr),
                                   Ia64KernelContextError::StorageRangesOverlap));

    const UInt64 memoryStackDataAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_memoryStack));
    const UInt64 rseBackingStoreDataAddress =
        static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_rseBackingStore));
    const auto memoryDataAliasedByRseVirtual = Storage(g_rseBackingStore, memoryStackDataAddress);
    ASTERIA_EXPECT(runner, "an RSE virtual range cannot alias memory-stack storage bytes",
                   failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, virtuallyAliasedMemoryStack,
                                                 memoryDataAliasedByRseVirtual, &Entry, nullptr),
                                   Ia64KernelContextError::StorageRangesOverlap));

    const auto rseDataAliasedByMemoryVirtual = Storage(g_memoryStack, rseBackingStoreDataAddress);
    const auto independentRseVirtual = Storage(g_rseBackingStore, RemoteVirtualStartB);
    ASTERIA_EXPECT(runner, "a memory-stack virtual range cannot alias RSE storage bytes",
                   failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, rseDataAliasedByMemoryVirtual,
                                                 independentRseVirtual, &Entry, nullptr),
                                   Ia64KernelContextError::StorageRangesOverlap));

    ASTERIA_EXPECT(
        runner, "a null entry descriptor is rejected after storage validation",
        failureIsAtomic(fresh.Prepare(AddressMostSignificantBit, memoryStack, rseBackingStore, nullptr, nullptr),
                        Ia64KernelContextError::InvalidEntry));

    const UInt64 aliasStorageAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(g_aliasStorage));
    const auto contextAliasIsAtomic = [](const WritableIa64KernelStorageView& aliasedMemoryStack,
                                         const WritableIa64KernelStorageView& aliasedRseBackingStore) noexcept
    {
        auto* const context = new (g_aliasStorage) Ia64KernelContext{};
        UInt8 expected[sizeof(Ia64KernelContext)]{};
        CopyBytes(context, expected, sizeof(*context));
        const Ia64KernelContextError error =
            context->Prepare(AddressMostSignificantBit, aliasedMemoryStack, aliasedRseBackingStore, &Entry, nullptr);
        const bool result = (error == Ia64KernelContextError::StorageRangesOverlap) &&
                            BytesAreEqual(context, expected, sizeof(*context));
        context->~Ia64KernelContext();
        return result;
    };

    ASTERIA_EXPECT(runner, "context metadata cannot reside in memory-stack storage bytes",
                   contextAliasIsAtomic(Storage(g_aliasStorage, RemoteVirtualStartA),
                                        Storage(g_rseBackingStore, RemoteVirtualStartB)));
    ASTERIA_EXPECT(runner, "context metadata cannot be covered by the memory-stack virtual range",
                   contextAliasIsAtomic(Storage(g_memoryStack, aliasStorageAddress),
                                        Storage(g_rseBackingStore, RemoteVirtualStartB)));
    ASTERIA_EXPECT(runner, "context metadata cannot reside in RSE backing-store bytes",
                   contextAliasIsAtomic(Storage(g_memoryStack, RemoteVirtualStartA),
                                        Storage(g_aliasStorage, RemoteVirtualStartB)));
    ASTERIA_EXPECT(runner, "context metadata cannot be covered by the RSE virtual range",
                   contextAliasIsAtomic(Storage(g_memoryStack, RemoteVirtualStartA),
                                        Storage(g_rseBackingStore, aliasStorageAddress)));

    ASTERIA_EXPECT(runner, "failed preparation does not modify either caller-owned storage range",
                   BytesHaveValue(g_memoryStack, StorageBytes, MemoryStackSentinel),
                   BytesHaveValue(g_rseBackingStore, StorageBytes, RseBackingStoreSentinel));
}

void TestPreparationAndSwitch(TestContext& runner) noexcept
{
    Ia64KernelRegisterState directCurrent{};
    Ia64KernelRegisterState directNext{};
    ResetAdapter();
    ASTERIA_EXPECT(runner, "the raw wrapper rejects one register image used as both owners",
                   SwitchIa64SynchronousContinuation(directCurrent, directCurrent) ==
                       Ia64SynchronousContinuationSwitchError::SameRegisterState,
                   g_switchCount == 0U);
    ASTERIA_EXPECT(runner, "the raw wrapper forwards two exact register-state owners",
                   SwitchIa64SynchronousContinuation(directCurrent, directNext) ==
                       Ia64SynchronousContinuationSwitchError::None,
                   g_switchCount == 1U, g_currentRegisters == &directCurrent, g_nextRegisters == &directNext);

    Ia64KernelContext current;
    Ia64KernelContext next;
    const auto memoryStack = Storage(g_memoryStack);
    const auto rseBackingStore = Storage(g_rseBackingStore);
    int argument{7};

    const Ia64KernelContextError prepared =
        next.Prepare(AddressMostSignificantBit, memoryStack, rseBackingStore, &Entry, &argument);
    const Ia64KernelContextSnapshot snapshot = next.GetSnapshot();
    const UInt64 entryAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(&Entry));
    const UInt64 argumentAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(&argument));
    const UInt64 startAddress = AsteriaIa64GetStartKernelContextAddress();
    ASTERIA_EXPECT(runner, "successful preparation publishes the exact owned context metadata",
                   prepared == Ia64KernelContextError::None, next.IsPrepared(),
                   snapshot.MemoryStackVirtualStart == memoryStack.VirtualStart,
                   snapshot.MemoryStackSizeBytes == memoryStack.SizeBytes,
                   snapshot.RseBackingStoreVirtualStart == rseBackingStore.VirtualStart,
                   snapshot.RseBackingStoreSizeBytes == rseBackingStore.SizeBytes,
                   snapshot.EntryDescriptorAddress == entryAddress, snapshot.EntryPointAddress == entryAddress,
                   snapshot.ArgumentAddress == argumentAddress, snapshot.State == Ia64KernelContextState::Prepared,
                   snapshot.ImplementedVirtualAddressMostSignificantBit == AddressMostSignificantBit);

    UInt8 preparedBeforeRetry[sizeof(Ia64KernelContext)]{};
    CopyBytes(&next, preparedBeforeRetry, sizeof(next));
    const Ia64KernelContextError preparedRetry =
        next.Prepare(AddressMostSignificantBit, memoryStack, rseBackingStore, &Entry, &argument);
    ASTERIA_EXPECT(runner, "a prepared context is single-use", preparedRetry == Ia64KernelContextError::AlreadyPrepared,
                   BytesAreEqual(&next, preparedBeforeRetry, sizeof(next)));

    UInt8 preparedBeforeSelfSwitch[sizeof(Ia64KernelContext)]{};
    CopyBytes(&next, preparedBeforeSelfSwitch, sizeof(next));
    const Ia64KernelContextError selfSwitch = next.SwitchTo(next);
    ASTERIA_EXPECT(runner, "a context cannot switch to itself", selfSwitch == Ia64KernelContextError::SameContext,
                   BytesAreEqual(&next, preparedBeforeSelfSwitch, sizeof(next)));

    Ia64KernelContext unprepared;
    UInt8 currentBeforeRejectedSwitch[sizeof(Ia64KernelContext)]{};
    UInt8 destinationBeforeRejectedSwitch[sizeof(Ia64KernelContext)]{};
    CopyBytes(&current, currentBeforeRejectedSwitch, sizeof(current));
    CopyBytes(&unprepared, destinationBeforeRejectedSwitch, sizeof(unprepared));
    const Ia64KernelContextError rejectedSwitch = current.SwitchTo(unprepared);
    ASTERIA_EXPECT(runner, "an unprepared destination is rejected without entering the adapter",
                   rejectedSwitch == Ia64KernelContextError::ContextNotPrepared,
                   BytesAreEqual(&current, currentBeforeRejectedSwitch, sizeof(current)),
                   BytesAreEqual(&unprepared, destinationBeforeRejectedSwitch, sizeof(unprepared)));

    ResetAdapter();
    const Ia64KernelContextError switched = current.SwitchTo(next);
    ASTERIA_EXPECT(runner, "a valid switch passes two distinct retained register images once",
                   switched == Ia64KernelContextError::None, current.IsPrepared(), g_switchCount == 1U,
                   g_currentRegisters != nullptr, g_nextRegisters != nullptr, g_currentRegisters != g_nextRegisters);
    ASTERIA_EXPECT(runner, "the fresh IA-64 register image binds stack, RSE, entry, and ABI defaults",
                   PreparedRegistersAreExact(*g_nextRegisters, memoryStack.VirtualStart + memoryStack.SizeBytes - 16U,
                                             rseBackingStore.VirtualStart, entryAddress, 0U, argumentAddress,
                                             startAddress));

    Ia64KernelContext alternate;
    const auto alternateMemoryStack = Storage(g_alternateMemoryStack);
    const auto alternateRseBackingStore = Storage(g_alternateRseBackingStore);
    ASTERIA_EXPECT(runner, "a second context can own independent memory and register stacks",
                   alternate.Prepare(AddressMostSignificantBit, alternateMemoryStack, alternateRseBackingStore, &Entry,
                                     nullptr) == Ia64KernelContextError::None);

    Ia64KernelContext maximum;
    const auto maximumMemoryStack = Storage(g_oversizedMemoryStack, 0U, MaximumIa64KernelMemoryStackSizeBytes);
    const auto maximumRseBackingStore =
        Storage(g_oversizedRseBackingStore, 0U, MaximumIa64KernelRseBackingStoreSizeBytes);
    const auto maximumPrepared =
        maximum.Prepare(AddressMostSignificantBit, maximumMemoryStack, maximumRseBackingStore, &Entry, nullptr);
    const auto maximumSnapshot = maximum.GetSnapshot();
    ASTERIA_EXPECT(runner, "the exact sixty-four-KiB stack and RSE maxima remain accepted",
                   maximumPrepared == Ia64KernelContextError::None, maximum.IsPrepared(),
                   maximumSnapshot.MemoryStackSizeBytes == MaximumIa64KernelMemoryStackSizeBytes,
                   maximumSnapshot.RseBackingStoreSizeBytes == MaximumIa64KernelRseBackingStoreSizeBytes);

    auto& nextExternalFrame = next.GetExternalPreemptionFrame();
    auto& currentExternalFrame = current.GetExternalPreemptionFrame();
    ASTERIA_EXPECT(runner, "each context owns one distinct cache-line-aligned persistent frame",
                   &nextExternalFrame == &static_cast<const Ia64KernelContext&>(next).GetExternalPreemptionFrame(),
                   &nextExternalFrame != &currentExternalFrame,
                   (reinterpret_cast<Foundation::UIntPointer>(&nextExternalFrame) & 63U) == 0U);

    UInt64 retainedToken{0U};
    ASTERIA_EXPECT(runner, "a prepared destination can own one retained external frame",
                   RetainExternalFrame(nextExternalFrame, retainedToken));
    UInt8 currentBeforeExternalSwitch[sizeof(current)]{};
    UInt8 nextBeforeExternalSwitch[sizeof(next)]{};
    CopyBytes(&current, currentBeforeExternalSwitch, sizeof(current));
    CopyBytes(&next, nextBeforeExternalSwitch, sizeof(next));
    ResetAdapter();
    ASTERIA_EXPECT(runner, "cooperative switching cannot restore a retained destination image",
                   current.SwitchTo(next) == Ia64KernelContextError::ExternalPreemptionFrameOwned, g_switchCount == 0U,
                   BytesAreEqual(&current, currentBeforeExternalSwitch, sizeof(current)),
                   BytesAreEqual(&next, nextBeforeExternalSwitch, sizeof(next)));
    ASTERIA_EXPECT(runner, "the hosted lifecycle releases the retained destination for reuse",
                   ReleaseExternalFrame(nextExternalFrame, retainedToken));

    UInt64 currentToken{0U};
    ASTERIA_EXPECT(runner, "the current context can model independent external-frame ownership",
                   RetainExternalFrame(currentExternalFrame, currentToken));
    CopyBytes(&current, currentBeforeExternalSwitch, sizeof(current));
    CopyBytes(&alternate, nextBeforeExternalSwitch, sizeof(alternate));
    ResetAdapter();
    ASTERIA_EXPECT(runner, "cooperative switching cannot abandon a target-owned current frame",
                   current.SwitchTo(alternate) == Ia64KernelContextError::ExternalPreemptionFrameOwned,
                   g_switchCount == 0U, BytesAreEqual(&current, currentBeforeExternalSwitch, sizeof(current)),
                   BytesAreEqual(&alternate, nextBeforeExternalSwitch, sizeof(alternate)));
    ASTERIA_EXPECT(runner, "the hosted lifecycle releases current-frame ownership exactly once",
                   ReleaseExternalFrame(currentExternalFrame, currentToken));

    auto& nextPageFaultFrame = next.GetPageFaultFrame();
    auto& currentPageFaultFrame = current.GetPageFaultFrame();
    ASTERIA_EXPECT(runner, "each context owns a distinct aligned synchronous page-fault frame",
                   &nextPageFaultFrame == &static_cast<const Ia64KernelContext&>(next).GetPageFaultFrame(),
                   &nextPageFaultFrame != &currentPageFaultFrame,
                   static_cast<const void*>(&nextPageFaultFrame) != static_cast<const void*>(&nextExternalFrame),
                   (reinterpret_cast<Foundation::UIntPointer>(&nextPageFaultFrame) & 63U) == 0U);

    nextPageFaultFrame.State = Ia64PageFaultFrameState::Capturing;
    CopyBytes(&current, currentBeforeExternalSwitch, sizeof(current));
    CopyBytes(&next, nextBeforeExternalSwitch, sizeof(next));
    ResetAdapter();
    ASTERIA_EXPECT(runner, "cooperative switching cannot restore a page-fault-owned destination",
                   current.SwitchTo(next) == Ia64KernelContextError::PageFaultFrameOwned, g_switchCount == 0U,
                   BytesAreEqual(&current, currentBeforeExternalSwitch, sizeof(current)),
                   BytesAreEqual(&next, nextBeforeExternalSwitch, sizeof(next)));
    nextPageFaultFrame.State = Ia64PageFaultFrameState::Inactive;

    nextPageFaultFrame.State = Ia64PageFaultFrameState::ResumeCommitted;
    CopyBytes(&next, nextBeforeExternalSwitch, sizeof(next));
    ResetAdapter();
    ASTERIA_EXPECT(runner, "cooperative switching cannot restore a destination committed to its rfi",
                   current.SwitchTo(next) == Ia64KernelContextError::PageFaultFrameOwned, g_switchCount == 0U,
                   BytesAreEqual(&current, currentBeforeExternalSwitch, sizeof(current)),
                   BytesAreEqual(&next, nextBeforeExternalSwitch, sizeof(next)));
    nextPageFaultFrame.State = Ia64PageFaultFrameState::Inactive;

    constexpr Ia64PageFaultFrameState ownedStates[]{
        Ia64PageFaultFrameState::Capturing,
        Ia64PageFaultFrameState::ResumeCommitted,
        static_cast<Ia64PageFaultFrameState>(4U),
    };
    for (const Ia64PageFaultFrameState owned : ownedStates)
    {
        currentPageFaultFrame.State = owned;
        CopyBytes(&current, currentBeforeExternalSwitch, sizeof(current));
        CopyBytes(&alternate, nextBeforeExternalSwitch, sizeof(alternate));
        ResetAdapter();
        ASTERIA_EXPECT(runner, "cooperative switching cannot abandon a frame the entry assembly owns",
                       current.SwitchTo(alternate) == Ia64KernelContextError::PageFaultFrameOwned, g_switchCount == 0U,
                       BytesAreEqual(&current, currentBeforeExternalSwitch, sizeof(current)),
                       BytesAreEqual(&alternate, nextBeforeExternalSwitch, sizeof(alternate)));
    }

    // A thread blocked in the user-fault bridge keeps its frame Retained
    // while it is switched away and while it is resumed.
    currentPageFaultFrame.State = Ia64PageFaultFrameState::Retained;
    nextPageFaultFrame.State = Ia64PageFaultFrameState::Retained;
    CopyBytes(&current, currentBeforeExternalSwitch, sizeof(current));
    CopyBytes(&next, nextBeforeExternalSwitch, sizeof(next));
    ResetAdapter();
    ASTERIA_EXPECT(runner, "a retained page-fault frame blocks neither leaving nor resuming its thread",
                   current.SwitchTo(next) == Ia64KernelContextError::None, g_switchCount == 1U,
                   currentPageFaultFrame.State == Ia64PageFaultFrameState::Retained,
                   nextPageFaultFrame.State == Ia64PageFaultFrameState::Retained);
    currentPageFaultFrame.State = Ia64PageFaultFrameState::Inactive;
    nextPageFaultFrame.State = Ia64PageFaultFrameState::Inactive;
}

void TestPageFaultFrameBinding(TestContext& runner) noexcept
{
    {
        alignas(64U) Ia64PageFaultFrame externalFrame{};
        alignas(64U) Ia64PageFaultFrame replayFrame{};
        alignas(64U) Ia64PageFaultFrame lateFrame{};
        alignas(64U) UInt8 misalignedStorage[sizeof(Ia64PageFaultFrame) + 64U]{};
        Ia64KernelContext context;
        Ia64KernelContext lateContext;

        Ia64PageFaultFrame* const embeddedFrame = &context.GetPageFaultFrame();
        const Foundation::UIntPointer contextStart = reinterpret_cast<Foundation::UIntPointer>(&context);
        const Foundation::UIntPointer contextEnd = contextStart + sizeof(context);
        const Foundation::UIntPointer embeddedStart = reinterpret_cast<Foundation::UIntPointer>(embeddedFrame);
        ASTERIA_EXPECT(runner, "a default context selects its private pristine page-fault frame",
                       embeddedFrame == &static_cast<const Ia64KernelContext&>(context).GetPageFaultFrame(),
                       embeddedStart >= contextStart, (embeddedStart + sizeof(*embeddedFrame)) <= contextEnd,
                       embeddedFrame->NextToken == 0U, embeddedFrame->State == Ia64PageFaultFrameState::Inactive);

        UInt8 expected[sizeof(context)]{};
        CopyBytes(&context, expected, sizeof(context));
        auto* const misalignedFrame =
            reinterpret_cast<Ia64PageFaultFrame*>(reinterpret_cast<Foundation::UIntPointer>(misalignedStorage) + 1U);
        auto* const aliasedFrame = reinterpret_cast<Ia64PageFaultFrame*>(&context.GetExternalPreemptionFrame());
        ASTERIA_EXPECT(runner, "null, misaligned, self, and context-alias bindings are atomic",
                       context.BindPageFaultFrame(nullptr) == Ia64PageFaultFrameBindingError::InvalidFrame,
                       context.BindPageFaultFrame(misalignedFrame) ==
                           Ia64PageFaultFrameBindingError::InvalidFrameAlignment,
                       context.BindPageFaultFrame(embeddedFrame) == Ia64PageFaultFrameBindingError::FrameAliasesContext,
                       context.BindPageFaultFrame(aliasedFrame) == Ia64PageFaultFrameBindingError::FrameAliasesContext,
                       BytesAreEqual(&context, expected, sizeof(context)));

        externalFrame.NextToken = 2U;
        ASTERIA_EXPECT(runner, "a stale-generation external frame cannot become a fresh binding",
                       context.BindPageFaultFrame(&externalFrame) ==
                           Ia64PageFaultFrameBindingError::ExternalFrameNotPristine,
                       &context.GetPageFaultFrame() == embeddedFrame);
        externalFrame.NextToken = 0U;
        externalFrame.State = Ia64PageFaultFrameState::Capturing;
        ASTERIA_EXPECT(runner, "a fail-stop owned external frame cannot be bound",
                       context.BindPageFaultFrame(&externalFrame) == Ia64PageFaultFrameBindingError::ExternalFrameOwned,
                       &context.GetPageFaultFrame() == embeddedFrame);
        externalFrame.State = Ia64PageFaultFrameState::Inactive;

        embeddedFrame->CaptureToken = 1U;
        ASTERIA_EXPECT(runner, "binding rejects non-pristine embedded fallback state",
                       context.BindPageFaultFrame(&externalFrame) ==
                           Ia64PageFaultFrameBindingError::EmbeddedFrameNotPristine,
                       &context.GetPageFaultFrame() == embeddedFrame);
        embeddedFrame->CaptureToken = 0U;
        embeddedFrame->State = Ia64PageFaultFrameState::Retained;
        ASTERIA_EXPECT(runner, "binding cannot abandon an owned embedded frame",
                       context.BindPageFaultFrame(&externalFrame) == Ia64PageFaultFrameBindingError::EmbeddedFrameOwned,
                       &context.GetPageFaultFrame() == embeddedFrame);
        embeddedFrame->State = Ia64PageFaultFrameState::Inactive;

        ASTERIA_EXPECT(runner, "one successful binding permanently selects the exact external frame",
                       context.BindPageFaultFrame(&externalFrame) == Ia64PageFaultFrameBindingError::None,
                       &context.GetPageFaultFrame() == &externalFrame,
                       &static_cast<const Ia64KernelContext&>(context).GetPageFaultFrame() == &externalFrame,
                       context.BindPageFaultFrame(&replayFrame) == Ia64PageFaultFrameBindingError::AlreadyBound,
                       &context.GetPageFaultFrame() == &externalFrame);

        Ia64PageFaultFrame* const lateEmbeddedFrame = &lateContext.GetPageFaultFrame();
        const auto latePrepared = lateContext.Prepare(AddressMostSignificantBit, Storage(g_memoryStack),
                                                      Storage(g_rseBackingStore), &Entry, nullptr);
        ASTERIA_EXPECT(runner, "a prepared context rejects late frame binding without replacement",
                       latePrepared == Ia64KernelContextError::None,
                       lateContext.BindPageFaultFrame(&lateFrame) ==
                           Ia64PageFaultFrameBindingError::ContextAlreadyPrepared,
                       &lateContext.GetPageFaultFrame() == lateEmbeddedFrame);
    }

    {
        alignas(64U) Ia64PageFaultFrame sourceFrame{};
        alignas(64U) Ia64PageFaultFrame destinationFrame{};
        Ia64KernelContext source;
        Ia64KernelContext destination;
        const bool bound = (source.BindPageFaultFrame(&sourceFrame) == Ia64PageFaultFrameBindingError::None) &&
                           (destination.BindPageFaultFrame(&destinationFrame) == Ia64PageFaultFrameBindingError::None);
        const auto prepared = destination.Prepare(AddressMostSignificantBit, Storage(g_alternateMemoryStack),
                                                  Storage(g_alternateRseBackingStore), &Entry, nullptr);

        destinationFrame.State = Ia64PageFaultFrameState::Capturing;
        ResetAdapter();
        const auto destinationOwned = source.SwitchTo(destination);
        destinationFrame.State = Ia64PageFaultFrameState::Inactive;
        sourceFrame.State = Ia64PageFaultFrameState::Capturing;
        const auto sourceOwned = source.SwitchTo(destination);
        sourceFrame.State = Ia64PageFaultFrameState::Inactive;
        ASTERIA_EXPECT(runner, "switch guards follow both exact externally bound page-fault frames", bound,
                       prepared == Ia64KernelContextError::None,
                       destinationOwned == Ia64KernelContextError::PageFaultFrameOwned,
                       sourceOwned == Ia64KernelContextError::PageFaultFrameOwned, g_switchCount == 0U);
    }
}

void TestExternalFrameIdleContinuation(TestContext& runner) noexcept
{
    alignas(64U) Ia64PageFaultFrame externalPageFaultFrame{};
    Ia64KernelContext idle;
    Ia64KernelContext worker;
    const auto idleStack = Storage(g_externalIdleMemoryStack);
    const auto idleRse = Storage(g_externalIdleRseBackingStore, 0U, sizeof(g_externalIdleRseBackingStore));
    int landingArgument{19};

    ASTERIA_EXPECT(runner, "the idle source selects its external page-fault corridor once",
                   idle.BindPageFaultFrame(&externalPageFaultFrame) == Ia64PageFaultFrameBindingError::None,
                   &idle.GetPageFaultFrame() == &externalPageFaultFrame);

    const auto freshSnapshot = idle.GetExternalFrameIdleContinuationSnapshot();
    ASTERIA_EXPECT(runner, "an unconfigured context cannot mint an idle-dispatch landing",
                   freshSnapshot.Configured == 0U, freshSnapshot.PublishedGeneration == 0U,
                   freshSnapshot.EntryGlobalPointer == 0U, !idle.SaveExternalFrameIdleContinuation(1U));

    UInt8 expected[sizeof(idle)]{};
    CopyBytes(&idle, expected, sizeof(idle));
    auto invalidStack = idleStack;
    invalidStack.SizeBytes -= 4096U;
    ASTERIA_EXPECT(runner, "invalid coordinator storage is rejected without context mutation",
                   idle.ConfigureExternalFrameIdleContinuation(AddressMostSignificantBit, invalidStack, idleRse, &Entry,
                                                               &landingArgument) ==
                       Ia64ExternalFrameIdleContinuationError::InvalidMemoryStack,
                   BytesAreEqual(&idle, expected, sizeof(idle)));

    const UInt64 entryAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(&Entry));
    constexpr UInt64 descriptorAddress{RemoteVirtualStartB + 0x2'0000U};
    constexpr UInt64 entryGlobalPointer{RemoteVirtualStartB + 0x3'0000U};
    const auto configured = idle.ConfigureExternalFrameIdleContinuation(AddressMostSignificantBit, idleStack, idleRse,
                                                                        Ia64KernelContextResolvedEntry{
                                                                            .DescriptorAddress = descriptorAddress,
                                                                            .EntryPointAddress = entryAddress,
                                                                            .GlobalPointer = entryGlobalPointer,
                                                                        },
                                                                        &landingArgument);
    const Ia64ExternalFrameIdleContinuationSnapshot snapshot = idle.GetExternalFrameIdleContinuationSnapshot();
    const UInt64 argumentAddress = static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(&landingArgument));
    ASTERIA_EXPECT(runner, "configuration retains one exact reusable processor corridor",
                   configured == Ia64ExternalFrameIdleContinuationError::None,
                   snapshot.MemoryStackVirtualStart == idleStack.VirtualStart,
                   snapshot.MemoryStackSizeBytes == idleStack.SizeBytes,
                   snapshot.RseBackingStoreVirtualStart == idleRse.VirtualStart,
                   snapshot.RseBackingStoreSizeBytes == idleRse.SizeBytes,
                   snapshot.EntryDescriptorAddress == descriptorAddress, snapshot.EntryPointAddress == entryAddress,
                   snapshot.EntryGlobalPointer == entryGlobalPointer, snapshot.ArgumentAddress == argumentAddress,
                   snapshot.PublishedGeneration == 0U, snapshot.Configured == 1U);
    ASTERIA_EXPECT(runner, "the idle-dispatch corridor cannot be rebound after publication",
                   idle.ConfigureExternalFrameIdleContinuation(AddressMostSignificantBit, idleStack, idleRse, &Entry,
                                                               &landingArgument) ==
                       Ia64ExternalFrameIdleContinuationError::AlreadyConfigured);
    ASTERIA_EXPECT(runner, "a fresh cooperative source cannot publish a resumable landing",
                   !idle.SaveExternalFrameIdleContinuation(1U));

    const auto workerStack = Storage(g_alternateMemoryStack);
    const auto workerRse = Storage(g_alternateRseBackingStore);
    ASTERIA_EXPECT(runner, "the landing fixture prepares one independent worker",
                   worker.Prepare(AddressMostSignificantBit, workerStack, workerRse, &Entry, nullptr) ==
                       Ia64KernelContextError::None);
    ResetAdapter();
    ASTERIA_EXPECT(runner, "the exact idle context first acquires a suspended native image",
                   idle.SwitchTo(worker) == Ia64KernelContextError::None, idle.IsPrepared(),
                   g_currentRegisters != nullptr);

    constexpr UInt64 firstGeneration{7U};
    ASTERIA_EXPECT(runner, "refresh replaces the suspended image with a dedicated landing",
                   idle.SaveExternalFrameIdleContinuation(firstGeneration),
                   idle.GetExternalFrameIdleContinuationGeneration() == firstGeneration,
                   PreparedRegistersAreExact(*g_currentRegisters, idleStack.VirtualStart + idleStack.SizeBytes - 16U,
                                             idleRse.VirtualStart, entryAddress, entryGlobalPointer, argumentAddress,
                                             AsteriaIa64GetStartKernelContextAddress()));

    CopyBytes(&idle, expected, sizeof(idle));
    ASTERIA_EXPECT(runner, "zero and replayed generations cannot re-enter a consumed action",
                   !idle.SaveExternalFrameIdleContinuation(firstGeneration),
                   !idle.SaveExternalFrameIdleContinuation(0U), BytesAreEqual(&idle, expected, sizeof(idle)));

    UInt64 frameToken{0U};
    ASTERIA_EXPECT(runner, "the fixture can retain independent architectural ownership",
                   RetainExternalFrame(idle.GetExternalPreemptionFrame(), frameToken));
    CopyBytes(&idle, expected, sizeof(idle));
    ASTERIA_EXPECT(runner, "refresh cannot overwrite a context that owns an external frame",
                   !idle.SaveExternalFrameIdleContinuation(firstGeneration + 1U),
                   BytesAreEqual(&idle, expected, sizeof(idle)));
    ASTERIA_EXPECT(runner, "the fixture releases exact architectural ownership",
                   ReleaseExternalFrame(idle.GetExternalPreemptionFrame(), frameToken));
    idle.GetPageFaultFrame().State = Ia64PageFaultFrameState::Capturing;
    CopyBytes(&idle, expected, sizeof(idle));
    ASTERIA_EXPECT(runner, "refresh cannot overwrite a page-fault-owned idle context",
                   !idle.SaveExternalFrameIdleContinuation(firstGeneration + 1U),
                   BytesAreEqual(&idle, expected, sizeof(idle)));
    idle.GetPageFaultFrame().State = Ia64PageFaultFrameState::Inactive;
    ASTERIA_EXPECT(runner, "a newer action supersedes the old dedicated landing generation",
                   idle.SaveExternalFrameIdleContinuation(firstGeneration + 1U),
                   idle.GetExternalFrameIdleContinuationGeneration() == (firstGeneration + 1U));
}
} // Namespace
} // Namespace Asteria::Test

extern "C" void
AsteriaIa64SwitchKernelContext(Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* const current,
                               const Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState* const next) noexcept
{
    ++Asteria::Test::g_switchCount;
    Asteria::Test::g_currentRegisters = current;
    Asteria::Test::g_nextRegisters = next;
}

namespace Asteria::Test
{
// A thread's f32-f127 area attaches once, to a context that has not been prepared; the target
// switch spills and fills it.
void TestHighFloatingArea(TestContext& runner) noexcept
{
    Ia64KernelContext fresh;
    Ia64KernelContext prepared;
    Platform::Ia64::Scheduler::Ia64HighFloatingArea area{};
    Platform::Ia64::Scheduler::Ia64HighFloatingArea other{};
    const bool attached = fresh.AttachHighFloatingArea(area);
    const bool again = fresh.AttachHighFloatingArea(other);
    const bool preparedFirst =
        prepared.Prepare(AddressMostSignificantBit, Storage(g_memoryStack), Storage(g_rseBackingStore), &Entry,
                         nullptr) == Ia64KernelContextError::None;
    ASTERIA_EXPECT(runner, "an f32-f127 area attaches once, before preparation", attached, !again, preparedFirst,
                   !prepared.AttachHighFloatingArea(other),
                   fresh.Prepare(AddressMostSignificantBit, Storage(g_memoryStack), Storage(g_rseBackingStore), &Entry,
                                 nullptr) == Ia64KernelContextError::None);
}
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64KernelContextTests"};
    Asteria::Test::TestHighFloatingArea(runner);
    Asteria::Test::TestValidation(runner);
    Asteria::Test::TestPreparationAndSwitch(runner);
    Asteria::Test::TestPageFaultFrameBinding(runner);
    Asteria::Test::TestExternalFrameIdleContinuation(runner);
    return runner.Finish();
}
