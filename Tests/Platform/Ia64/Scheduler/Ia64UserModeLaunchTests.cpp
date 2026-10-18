// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"

#include "Support/TestContext.hpp"
#include <cstdio>
#include <cstring>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt64;
using Platform::Ia64::Scheduler::Ia64UserModeCurrentProcessorStatusMask;
using Platform::Ia64::Scheduler::Ia64UserModeCurrentProcessorStatusValue;
using Platform::Ia64::Scheduler::Ia64UserModeInitialFloatingPointStatus;
using Platform::Ia64::Scheduler::Ia64UserModeInitialProcessorStatus;
using Platform::Ia64::Scheduler::Ia64UserModeInitialRseConfiguration;
using Platform::Ia64::Scheduler::Ia64UserModeInitialStackScratchBytes;
using Platform::Ia64::Scheduler::Ia64UserModeLaunch;
using Platform::Ia64::Scheduler::Ia64UserModeLaunchDisposition;
using Platform::Ia64::Scheduler::Ia64UserModeLaunchError;
using Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan;
using Platform::Ia64::Scheduler::Ia64UserModeLaunchRequest;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuation;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationOwner;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState;
using Platform::Ia64::Scheduler::Ia64UserModeVirtualRange;

namespace
{
constexpr UInt8 ImplementedAddressMostSignificantBit{53U};
constexpr UInt64 StorageBytes{16U * 1024U};
constexpr UInt64 UserCodeStart{0x2000'0000'0100'1000ULL};
constexpr UInt64 UserStackStart{0x2000'0000'0200'1000ULL};
constexpr UInt64 UserRseStart{0x2000'0000'0300'1000ULL};
constexpr UInt64 InitialGlobalPointer{0x2000'0000'0400'1800ULL};
constexpr UInt64 InitialRegister4{0x0123'4567'89AB'CDEFULL};
constexpr UInt64 InitialRegister5{0xFEDC'BA98'7654'3210ULL};
constexpr UInt64 InitialRegister6{0x0F1E'2D3C'4B5A'6978ULL};
constexpr UInt64 InitialRegister7{0x8796'A5B4'C3D2'E1F0ULL};

alignas(4096U) UInt8 g_supervisorMemoryStack[StorageBytes]{};
alignas(4096U) UInt8 g_supervisorRseBackingStore[StorageBytes]{};
alignas(16U) Ia64UserModeSupervisorContinuation g_continuation{};

Size g_adapterCallCount{0U};
UInt64 g_adapterStatus{3U};
bool g_adapterMarksResumePending{false};
Ia64UserModeLaunchPlan g_adapterPlan{};

[[nodiscard]] UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

void ResetContinuation() noexcept
{
    g_continuation = {};
    g_continuation.MemoryStack = Ia64UserModeVirtualRange{
        .VirtualStart = AddressOf(g_supervisorMemoryStack),
        .SizeBytes = StorageBytes,
    };
    g_continuation.RseBackingStore = Ia64UserModeVirtualRange{
        .VirtualStart = AddressOf(g_supervisorRseBackingStore),
        .SizeBytes = StorageBytes,
    };
}

[[nodiscard]] Ia64UserModeLaunchRequest MakeRequest() noexcept
{
    ResetContinuation();
    return Ia64UserModeLaunchRequest{
        .Code =
            {
                .VirtualStart = UserCodeStart,
                .SizeBytes = 4096U,
            },
        .MemoryStack =
            {
                .VirtualStart = UserStackStart,
                .SizeBytes = StorageBytes,
            },
        .RseBackingStore =
            {
                .VirtualStart = UserRseStart,
                .SizeBytes = StorageBytes,
            },
        .EntryBundleAddress = UserCodeStart,
        .InitialGlobalPointer = InitialGlobalPointer,
        .InitialArguments =
            {
                .Register4 = InitialRegister4,
                .Register5 = InitialRegister5,
                .Register6 = InitialRegister6,
                .Register7 = InitialRegister7,
            },
        .Requirements =
            {
                .CurrentProcessorStatusMask = Ia64UserModeCurrentProcessorStatusMask,
                .CurrentProcessorStatusValue = Ia64UserModeCurrentProcessorStatusValue,
                .UserProcessorStatus = Ia64UserModeInitialProcessorStatus,
                .ImplementedVirtualAddressMostSignificantBit = ImplementedAddressMostSignificantBit,
                .CurrentPrivilegeLevel = 0U,
                .CurrentVirtualMachineMode = 0U,
                .CurrentInstructionSet = 0U,
                .CurrentRegisterBank = 1U,
                .Reserved = {},
            },
        .SupervisorContinuation = &g_continuation,
        .Reserved = 0U,
    };
}

void ExpectValidationError(TestContext& runner, const Ia64UserModeLaunchRequest& request,
                           const Ia64UserModeLaunchError expected, const char* const description) noexcept
{
    UInt8 before[sizeof(g_continuation)]{};
    static_cast<void>(std::memcpy(before, &g_continuation, sizeof(g_continuation)));
    const auto result = Ia64UserModeLaunch::Validate(request);
    ASTERIA_EXPECT(runner, description, !result.HasValue(), result.GetError() == expected,
                   std::memcmp(before, &g_continuation, sizeof(g_continuation)) == 0);
}

void TestCanonicalPlan(TestContext& runner) noexcept
{
    const Ia64UserModeLaunchRequest request = MakeRequest();
    const auto result = Ia64UserModeLaunch::Validate(request);
    ASTERIA_EXPECT(runner, "the canonical Region-1 request validates", result.HasValue());
    if (!result.HasValue())
    {
        return;
    }

    const Ia64UserModeLaunchPlan& plan = result.GetValue();
    ASTERIA_EXPECT(
        runner, "validation emits the exact fixed native launch plan", plan.EntryBundleAddress == UserCodeStart,
        plan.UserStackPointer == (UserStackStart + StorageBytes - Ia64UserModeInitialStackScratchBytes),
        plan.UserRseBackingStorePointer == UserRseStart, plan.UserProcessorStatus == Ia64UserModeInitialProcessorStatus,
        plan.UserRseConfiguration == Ia64UserModeInitialRseConfiguration,
        plan.CurrentProcessorStatusMask == Ia64UserModeCurrentProcessorStatusMask,
        plan.CurrentProcessorStatusValue == Ia64UserModeCurrentProcessorStatusValue,
        plan.SupervisorContinuationAddress == AddressOf(&g_continuation),
        plan.InitialArguments.Register4 == InitialRegister4, plan.InitialArguments.Register5 == InitialRegister5,
        plan.InitialArguments.Register6 == InitialRegister6, plan.InitialArguments.Register7 == InitialRegister7,
        plan.InitialGlobalPointer == InitialGlobalPointer);

    // Prevent a user br.ia from entering an ISA that the native interruption entry cannot capture.
    ASTERIA_EXPECT(runner, "every user launch turns IA-32 transitions into supervised native exceptions",
                   (Ia64UserModeInitialProcessorStatus & (UInt64{1U} << 22U)) != 0U,
                   (Platform::Ia64::Scheduler::Ia64UserModePreemptibleProcessorStatus & (UInt64{1U} << 22U)) != 0U);
    constexpr UInt64 instructionTranslationBit{UInt64{1U} << 36U};
    constexpr UInt64 dataTranslationBit{UInt64{1U} << 17U};
    constexpr UInt64 rseTranslationBit{UInt64{1U} << 27U};
    constexpr UInt64 interruptionCollectionBit{UInt64{1U} << 13U};
    constexpr UInt64 interruptionDeliveryBit{UInt64{1U} << 14U};
    constexpr UInt64 lowFloatingDisableBit{UInt64{1U} << 18U};
    constexpr UInt64 highFloatingDisableBit{UInt64{1U} << 19U};
    constexpr UInt64 instructionSetBit{UInt64{1U} << 34U};
    constexpr UInt64 registerBankBit{UInt64{1U} << 44U};
    constexpr UInt64 currentPrivilegeMask{UInt64{3U} << 32U};
    ASTERIA_EXPECT(runner, "the target is translated PL3/BN1 with IC on, I off, and FP disabled",
                   (Ia64UserModeInitialProcessorStatus & currentPrivilegeMask) == currentPrivilegeMask,
                   (Ia64UserModeInitialProcessorStatus &
                    (instructionTranslationBit | dataTranslationBit | rseTranslationBit | interruptionCollectionBit |
                     lowFloatingDisableBit | highFloatingDisableBit | registerBankBit)) ==
                       (instructionTranslationBit | dataTranslationBit | rseTranslationBit | interruptionCollectionBit |
                        lowFloatingDisableBit | highFloatingDisableBit | registerBankBit),
                   (Ia64UserModeInitialProcessorStatus & (interruptionDeliveryBit | instructionSetBit)) == 0U,
                   (Ia64UserModeCurrentProcessorStatusMask & (instructionSetBit | registerBankBit)) == 0U,
                   (Ia64UserModeCurrentProcessorStatusMask & (lowFloatingDisableBit | highFloatingDisableBit)) ==
                       (lowFloatingDisableBit | highFloatingDisableBit),
                   (Ia64UserModeCurrentProcessorStatusValue & (lowFloatingDisableBit | highFloatingDisableBit)) == 0U,
                   Ia64UserModeInitialFloatingPointStatus == 0x0009'804C'0270'033FULL);
}

void TestInitialArguments(TestContext& runner) noexcept
{
    auto request = MakeRequest();
    request.InitialArguments = {};
    const auto defaultResult = Ia64UserModeLaunch::Validate(request);
    ASTERIA_EXPECT(runner, "default initial arguments deliver zero in user r4 through r7", defaultResult.HasValue(),
                   defaultResult.GetValue().InitialArguments.Register4 == 0U,
                   defaultResult.GetValue().InitialArguments.Register5 == 0U,
                   defaultResult.GetValue().InitialArguments.Register6 == 0U,
                   defaultResult.GetValue().InitialArguments.Register7 == 0U);

    request = MakeRequest();
    request.InitialArguments.Register4 = ~UInt64{0U};
    request.InitialArguments.Register5 = ~UInt64{0U};
    request.InitialArguments.Register6 = ~UInt64{0U};
    request.InitialArguments.Register7 = ~UInt64{0U};
    const auto maximumResult = Ia64UserModeLaunch::Validate(request);
    ASTERIA_EXPECT(runner, "every four-word bit pattern is a valid initial argument contract", maximumResult.HasValue(),
                   maximumResult.GetValue().InitialArguments.Register4 == ~UInt64{0U},
                   maximumResult.GetValue().InitialArguments.Register5 == ~UInt64{0U},
                   maximumResult.GetValue().InitialArguments.Register6 == ~UInt64{0U},
                   maximumResult.GetValue().InitialArguments.Register7 == ~UInt64{0U});

    request = MakeRequest();
    request.Reserved = 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidProcessorStatusRequirements,
                          "the reserved request word remains invalid beside initial arguments");
}

void TestInitialGlobalPointer(TestContext& runner) noexcept
{
    auto request = MakeRequest();
    request.InitialGlobalPointer = 0U;
    const auto zeroResult = Ia64UserModeLaunch::Validate(request);
    ASTERIA_EXPECT(runner, "the relocation-free probe may launch with a zero global pointer", zeroResult.HasValue(),
                   zeroResult.GetValue().InitialGlobalPointer == 0U);

    request = MakeRequest();
    request.InitialGlobalPointer += 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidGlobalPointer,
                          "a misaligned initial global pointer is rejected");

    request = MakeRequest();
    request.InitialGlobalPointer = 0x0000'0000'0400'1800ULL;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidGlobalPointer,
                          "a nonzero Region-0 initial global pointer is rejected");

    request = MakeRequest();
    request.InitialGlobalPointer |= UInt64{1U} << 54U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidGlobalPointer,
                          "a noncanonical initial global pointer is rejected");

    request = MakeRequest();
    request.InitialGlobalPointer = ~UInt64{7U};
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidGlobalPointer,
                          "an initial global-pointer extent cannot wrap the address space");
}

void TestAddressValidation(TestContext& runner) noexcept
{
    auto request = MakeRequest();
    request.Requirements.ImplementedVirtualAddressMostSignificantBit = 49U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidAddressProfile,
                          "an unsupported virtual-address width is rejected");

    request = MakeRequest();
    request.Code.VirtualStart = 0x1000U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidCodeRange, "Region-0 user code is rejected");

    request = MakeRequest();
    request.EntryBundleAddress += 8U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidEntryBundle,
                          "a non-bundle-aligned entry is rejected");

    request = MakeRequest();
    request.EntryBundleAddress = request.Code.VirtualStart + request.Code.SizeBytes;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidEntryBundle,
                          "an entry outside the complete code range is rejected");

    request = MakeRequest();
    request.MemoryStack.SizeBytes = StorageBytes - 4096U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidMemoryStack,
                          "a memory stack below the P64 minimum is rejected");

    request = MakeRequest();
    request.RseBackingStore.VirtualStart += 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidRseBackingStore,
                          "an unaligned RSE backing store is rejected");

    request = MakeRequest();
    request.RseBackingStore.VirtualStart = request.MemoryStack.VirtualStart;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::UserRangesOverlap,
                          "user memory-stack and RSE ranges must be distinct");
}

void TestRequirementValidation(TestContext& runner) noexcept
{
    auto request = MakeRequest();
    request.Requirements.CurrentProcessorStatusMask ^= 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidProcessorStatusRequirements,
                          "the observable PSR contract is exact");

    request = MakeRequest();
    request.Requirements.CurrentRegisterBank = 0U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidProcessorStatusRequirements,
                          "the trusted current bank-one invariant is mandatory");

    request = MakeRequest();
    request.Requirements.CurrentInstructionSet = 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidProcessorStatusRequirements,
                          "the trusted native instruction-set invariant is mandatory");

    request = MakeRequest();
    request.Requirements.Reserved[2U] = 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidProcessorStatusRequirements,
                          "reserved requirement bytes must be zero");
}

void TestContinuationValidation(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "continuation states and owners preserve their native ABI",
                   static_cast<UInt64>(Ia64UserModeSupervisorContinuationState::Fresh) == 0U,
                   static_cast<UInt64>(Ia64UserModeSupervisorContinuationState::Retained) == 1U,
                   static_cast<UInt64>(Ia64UserModeSupervisorContinuationOwner::None) == 0U,
                   static_cast<UInt64>(Ia64UserModeSupervisorContinuationOwner::SystemCallWait) == 1U,
                   static_cast<UInt64>(Ia64UserModeSupervisorContinuationOwner::PageFault) == 2U);

    auto request = MakeRequest();
    request.SupervisorContinuation = nullptr;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidSupervisorContinuation,
                          "a null supervisor continuation is rejected");

    request = MakeRequest();
    g_continuation.State = Ia64UserModeSupervisorContinuationState::Retained;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidSupervisorContinuation,
                          "a retained continuation cannot be reused");

    request = MakeRequest();
    g_continuation.Owner = Ia64UserModeSupervisorContinuationOwner::PageFault;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidSupervisorContinuation,
                          "a continuation already owned by another entry path is rejected");

    request = MakeRequest();
    g_continuation.Registers.StackPointer = 1U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidSupervisorContinuation,
                          "all register-image bytes must be fresh before capture");

    request = MakeRequest();
    g_continuation.MemoryStack.SizeBytes = StorageBytes - 4096U;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::InvalidSupervisorMemoryStack,
                          "the retained supervisor memory stack is bounded");

    request = MakeRequest();
    g_continuation.RseBackingStore.VirtualStart = g_continuation.MemoryStack.VirtualStart;
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::SupervisorRangesOverlap,
                          "supervisor memory-stack and RSE ranges must be distinct");

    request = MakeRequest();
    const UInt64 continuationPage = AddressOf(&g_continuation) & ~UInt64{4095U};
    g_continuation.MemoryStack = Ia64UserModeVirtualRange{
        .VirtualStart = continuationPage,
        .SizeBytes = StorageBytes,
    };
    g_continuation.RseBackingStore = Ia64UserModeVirtualRange{
        .VirtualStart = 0x0001'0000'0000'0000ULL,
        .SizeBytes = StorageBytes,
    };
    ExpectValidationError(runner, request, Ia64UserModeLaunchError::SupervisorContinuationAliasesStorage,
                          "the continuation object cannot alias retained supervisor storage");
}

void TestLaunchBoundary(TestContext& runner) noexcept
{
    g_adapterCallCount = 0U;
    g_adapterMarksResumePending = false;
    auto request = MakeRequest();
    request.EntryBundleAddress += 8U;
    const auto invalid = Ia64UserModeLaunch::Launch(request);
    ASTERIA_EXPECT(runner, "pure rejection does not enter the native adapter",
                   invalid == Ia64UserModeLaunchError::InvalidEntryBundle, g_adapterCallCount == 0U);

    struct StatusCase
    {
        UInt64 AdapterStatus;
        Ia64UserModeLaunchError Expected;
    };
    constexpr StatusCase cases[]{{1U, Ia64UserModeLaunchError::UnsupportedProcessorState},
                                 {2U, Ia64UserModeLaunchError::InvalidActiveSupervisorState},
                                 {3U, Ia64UserModeLaunchError::NativeTransitionUnavailable},
                                 {0U, Ia64UserModeLaunchError::UnexpectedAdapterReturn},
                                 {77U, Ia64UserModeLaunchError::UnexpectedAdapterReturn}};
    for (const StatusCase& statusCase : cases)
    {
        request = MakeRequest();
        UInt8 before[sizeof(g_continuation)]{};
        static_cast<void>(std::memcpy(before, &g_continuation, sizeof(g_continuation)));
        const Size previousCalls = g_adapterCallCount;
        g_adapterStatus = statusCase.AdapterStatus;
        const auto result = Ia64UserModeLaunch::Launch(request);
        ASTERIA_EXPECT(runner, "every hosted adapter return is an explicit non-transition error",
                       result == statusCase.Expected, g_adapterCallCount == (previousCalls + 1U),
                       g_adapterPlan.UserStackPointer ==
                           (UserStackStart + StorageBytes - Ia64UserModeInitialStackScratchBytes),
                       g_adapterPlan.InitialArguments.Register4 == InitialRegister4,
                       g_adapterPlan.InitialArguments.Register5 == InitialRegister5,
                       g_adapterPlan.InitialArguments.Register6 == InitialRegister6,
                       g_adapterPlan.InitialArguments.Register7 == InitialRegister7,
                       g_adapterPlan.InitialGlobalPointer == InitialGlobalPointer,
                       std::memcmp(before, &g_continuation, sizeof(g_continuation)) == 0);
    }
}

void TestResumableLaunchBoundary(TestContext& runner) noexcept
{
    constexpr UInt64 arbitraryAdapterStatuses[]{0U, 1U, 2U, 3U, 77U, ~UInt64{0U}};
    g_adapterMarksResumePending = true;
    for (const UInt64 status : arbitraryAdapterStatuses)
    {
        auto request = MakeRequest();
        g_adapterStatus = status;
        const Size previousCalls = g_adapterCallCount;
        const auto result = Ia64UserModeLaunch::LaunchResumable(request);
        ASTERIA_EXPECT(runner, "explicit ResumePending state wins before arbitrary r8 classification",
                       result.HasValue(), result.GetValue() == Ia64UserModeLaunchDisposition::SupervisorResumed,
                       g_adapterCallCount == (previousCalls + 1U),
                       g_continuation.State == Ia64UserModeSupervisorContinuationState::ResumePending);
    }
    g_adapterMarksResumePending = false;

    auto request = MakeRequest();
    g_adapterStatus = 1U;
    const auto ordinary = Ia64UserModeLaunch::LaunchResumable(request);
    ASTERIA_EXPECT(runner, "an ordinary adapter return still classifies r8 while state is Fresh", !ordinary.HasValue(),
                   ordinary.GetError() == Ia64UserModeLaunchError::UnsupportedProcessorState,
                   g_continuation.State == Ia64UserModeSupervisorContinuationState::Fresh);
}
} // Namespace
} // Namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64LaunchUserMode(const Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan* const plan) noexcept
{
    ++Asteria::Test::g_adapterCallCount;
    Asteria::Test::g_adapterPlan = *plan;
    if (Asteria::Test::g_adapterMarksResumePending)
    {
        auto* const continuation =
            reinterpret_cast<Asteria::Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuation*>(
                plan->SupervisorContinuationAddress);
        continuation->State =
            Asteria::Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState::ResumePending;
    }
    return Asteria::Test::g_adapterStatus;
}

int main()
{
    TestContext runner{"Ia64UserModeLaunchTests"};
    Asteria::Test::TestCanonicalPlan(runner);
    Asteria::Test::TestInitialArguments(runner);
    Asteria::Test::TestInitialGlobalPointer(runner);
    Asteria::Test::TestAddressValidation(runner);
    Asteria::Test::TestRequirementValidation(runner);
    Asteria::Test::TestContinuationValidation(runner);
    Asteria::Test::TestLaunchBoundary(runner);
    Asteria::Test::TestResumableLaunchBoundary(runner);
    return runner.Finish();
}
