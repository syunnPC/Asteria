// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Scheduler/Amd64KernelContext.hpp"

#include <new>

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContext;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContextError;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContextState;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelRegisterState;
using Asteria::Platform::Amd64::Scheduler::WritableAmd64KernelStackView;

namespace
{
alignas(4096) UInt8 g_stack[16U * 1024U]{};
alignas(4096) UInt8 g_secondStack[16U * 1024U]{};
alignas(4096) UInt8 g_aliasStorage[16U * 1024U]{};
int g_switchCalls{0};
Amd64KernelRegisterState* g_currentRegisters{nullptr};
const Amd64KernelRegisterState* g_nextRegisters{nullptr};

void Entry(void*) noexcept
{
}

[[nodiscard]] WritableAmd64KernelStackView StackView(UInt8* const data, const UInt64 size = sizeof(g_stack)) noexcept
{
    return WritableAmd64KernelStackView{
        .Data = data,
        .SizeBytes = size,
        .VirtualStart = static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(data)),
    };
}

[[nodiscard]] bool SnapshotIsFresh(const Amd64KernelContext& context) noexcept
{
    const auto snapshot = context.GetSnapshot();
    return (snapshot.StackVirtualStart == 0U) && (snapshot.StackSizeBytes == 0U) && (snapshot.EntryAddress == 0U) &&
           (snapshot.ArgumentAddress == 0U) && (snapshot.State == Amd64KernelContextState::Fresh);
}
} // namespace

extern "C" void AsteriaAmd64SwitchKernelContext(Amd64KernelRegisterState* const current,
                                                const Amd64KernelRegisterState* const next) noexcept
{
    ++g_switchCalls;
    g_currentRegisters = current;
    g_nextRegisters = next;
}

int main()
{
    TestContext runner{"Amd64KernelContextTests"};

    Amd64KernelContext nullEntry;
    ASTERIA_EXPECT(runner, "null entry is rejected",
                   nullEntry.Prepare(StackView(g_stack), nullptr, nullptr) == Amd64KernelContextError::InvalidEntry);
    ASTERIA_EXPECT(runner, "entry failure is atomic", SnapshotIsFresh(nullEntry));

    Amd64KernelContext nullStack;
    auto invalidStack = StackView(g_stack);
    invalidStack.Data = nullptr;
    ASTERIA_EXPECT(runner, "null stack is rejected",
                   nullStack.Prepare(invalidStack, &Entry, nullptr) == Amd64KernelContextError::InvalidStack);
    ASTERIA_EXPECT(runner, "stack failure is atomic", SnapshotIsFresh(nullStack));

    Amd64KernelContext shortStack;
    ASTERIA_EXPECT(runner, "short stack is rejected",
                   shortStack.Prepare(StackView(g_stack, 2048U), &Entry, nullptr) ==
                       Amd64KernelContextError::InvalidStack);

    Amd64KernelContext unalignedStack;
    auto unalignedView = StackView(g_stack);
    ++unalignedView.VirtualStart;
    ASTERIA_EXPECT(runner, "unaligned virtual stack is rejected",
                   unalignedStack.Prepare(unalignedView, &Entry, nullptr) == Amd64KernelContextError::InvalidStack);

    Amd64KernelContext unalignedData;
    auto unalignedDataView = StackView(g_stack + 1U);
    ASTERIA_EXPECT(runner, "unaligned writable alias is rejected",
                   unalignedData.Prepare(unalignedDataView, &Entry, nullptr) == Amd64KernelContextError::InvalidStack);

    Amd64KernelContext wrappingStack;
    auto wrappingView = StackView(g_stack, 4096U);
    wrappingView.VirtualStart = 0xFFFF'FFFF'FFFF'F000ULL;
    ASTERIA_EXPECT(runner, "wrapping virtual stack is rejected",
                   wrappingStack.Prepare(wrappingView, &Entry, nullptr) == Amd64KernelContextError::InvalidStack);

    Amd64KernelContext noncanonicalStack;
    auto noncanonicalView = StackView(g_stack, 4096U);
    noncanonicalView.VirtualStart = 0x0000'8000'0000'0000ULL;
    ASTERIA_EXPECT(runner, "noncanonical virtual stack is rejected",
                   noncanonicalStack.Prepare(noncanonicalView, &Entry, nullptr) ==
                       Amd64KernelContextError::InvalidStack);

    auto* const aliasedContext = ::new (static_cast<void*>(g_aliasStorage)) Amd64KernelContext{};
    ASTERIA_EXPECT(runner, "context storage cannot alias its writable stack",
                   aliasedContext->Prepare(StackView(g_aliasStorage), &Entry, nullptr) ==
                       Amd64KernelContextError::InvalidStack);
    ASTERIA_EXPECT(runner, "alias rejection leaves context fresh", SnapshotIsFresh(*aliasedContext));
    ASTERIA_EXPECT(runner, "external landing storage cannot overlap its owner",
                   aliasedContext->ConfigureExternalFrameIdleContinuation(StackView(g_aliasStorage), Entry, nullptr) ==
                       Amd64KernelContextError::InvalidStack);
    auto landingAlias = StackView(g_secondStack);
    landingAlias.VirtualStart = StackView(g_aliasStorage).VirtualStart;
    ASTERIA_EXPECT(runner, "external landing virtual mapping cannot overlap its owner",
                   aliasedContext->ConfigureExternalFrameIdleContinuation(landingAlias, Entry, nullptr) ==
                       Amd64KernelContextError::InvalidStack);
    ASTERIA_EXPECT(runner, "rejected landing cannot publish a generation",
                   !aliasedContext->SaveExternalFrameIdleContinuation(1U),
                   aliasedContext->GetExternalFrameIdleContinuationGeneration() == 0U);
    aliasedContext->~Amd64KernelContext();

    int argument{42};
    Amd64KernelContext prepared;
    ASTERIA_EXPECT(runner, "valid context is prepared",
                   prepared.Prepare(StackView(g_stack), &Entry, &argument) == Amd64KernelContextError::None);
    const auto preparedSnapshot = prepared.GetSnapshot();
    ASTERIA_EXPECT(runner, "prepared snapshot retains exact inputs",
                   preparedSnapshot.StackVirtualStart ==
                       static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(g_stack)),
                   preparedSnapshot.StackSizeBytes == sizeof(g_stack),
                   preparedSnapshot.EntryAddress ==
                       static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(&Entry)),
                   preparedSnapshot.ArgumentAddress ==
                       static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(&argument)),
                   preparedSnapshot.State == Amd64KernelContextState::Prepared);
    ASTERIA_EXPECT(runner, "preparation is single-use",
                   prepared.Prepare(StackView(g_secondStack), &Entry, nullptr) ==
                       Amd64KernelContextError::AlreadyPrepared);

    Amd64KernelContext current;
    Amd64KernelContext freshNext;
    ASTERIA_EXPECT(runner, "fresh next context is rejected",
                   current.SwitchTo(freshNext) == Amd64KernelContextError::ContextNotPrepared);
    ASTERIA_EXPECT(runner, "failed switch leaves current fresh", !current.IsPrepared());
    ASTERIA_EXPECT(runner, "self switch is rejected",
                   prepared.SwitchTo(prepared) == Amd64KernelContextError::SameContext);

    ASTERIA_EXPECT(runner, "prepared next context is accepted",
                   current.SwitchTo(prepared) == Amd64KernelContextError::None);
    ASTERIA_EXPECT(runner, "switch publishes distinct register blocks to adapter", current.IsPrepared(),
                   g_switchCalls == 1, g_currentRegisters != nullptr, g_nextRegisters != nullptr,
                   g_currentRegisters != g_nextRegisters);

    ASTERIA_EXPECT(runner, "unconfigured continuation cannot publish a generation",
                   !prepared.SaveExternalFrameIdleContinuation(1U),
                   prepared.GetExternalFrameIdleContinuationGeneration() == 0U);
    ASTERIA_EXPECT(runner, "external idle stack must be distinct",
                   prepared.ConfigureExternalFrameIdleContinuation(StackView(g_stack), Entry, nullptr) ==
                       Amd64KernelContextError::InvalidStack);
    ASTERIA_EXPECT(runner, "prepare dedicated fresh idle landing",
                   prepared.ConfigureExternalFrameIdleContinuation(StackView(g_secondStack), Entry, nullptr) ==
                       Amd64KernelContextError::None);
    ASTERIA_EXPECT(runner, "save publishes the new idle stack and exact generation",
                   prepared.SaveExternalFrameIdleContinuation(1U),
                   prepared.GetExternalFrameIdleContinuationGeneration() == 1U,
                   prepared.GetSnapshot().StackVirtualStart == StackView(g_secondStack).VirtualStart);
    ASTERIA_EXPECT(runner, "zero and replayed generations leave the landing unchanged",
                   !prepared.SaveExternalFrameIdleContinuation(0U), !prepared.SaveExternalFrameIdleContinuation(1U),
                   prepared.GetExternalFrameIdleContinuationGeneration() == 1U);
    ASTERIA_EXPECT(runner, "later frame generation reuses the dedicated landing",
                   prepared.SaveExternalFrameIdleContinuation(2U),
                   prepared.GetExternalFrameIdleContinuationGeneration() == 2U);
    ASTERIA_EXPECT(runner, "cannot retarget an armed continuation",
                   prepared.ConfigureExternalFrameIdleContinuation(StackView(g_aliasStorage), Entry, nullptr) ==
                       Amd64KernelContextError::AlreadyPrepared);

    return runner.Finish();
}
