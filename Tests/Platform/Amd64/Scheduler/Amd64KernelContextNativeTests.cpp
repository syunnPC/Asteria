// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Scheduler/Amd64KernelContext.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContext;
using Asteria::Platform::Amd64::Scheduler::Amd64ExtendedStateArea;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContextError;
using Asteria::Platform::Amd64::Scheduler::WritableAmd64KernelStackView;

extern "C" UInt64 AsteriaTestAmd64SwitchContexts(Amd64KernelContext* current, Amd64KernelContext* next) noexcept
{
    return static_cast<UInt64>(current->SwitchTo(*next));
}

extern "C" UInt64 AsteriaTestAmd64SwitchWithSentinels(Amd64KernelContext* current, Amd64KernelContext* next,
                                                      const UInt64* sentinels, UInt64* observed) noexcept;

extern "C" void AsteriaTestAmd64WorkerEntry(void* argument) noexcept;
extern "C" void AsteriaTestAmd64WorkerBody(void* argument) noexcept;
extern "C" UInt64 AsteriaTestAmd64EntryStackPointer;
extern "C" void* AsteriaTestAmd64EntryArgument;

UInt64 AsteriaTestAmd64EntryStackPointer{0U};
void* AsteriaTestAmd64EntryArgument{nullptr};

__asm__(".text\n"
        ".p2align 4\n"
        ".local AsteriaTestAmd64SwitchWithSentinels\n"
        ".type AsteriaTestAmd64SwitchWithSentinels, @function\n"
        "AsteriaTestAmd64SwitchWithSentinels:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $8, %rsp\n"
        "    movq %rcx, 0(%rsp)\n"
        "    movq 0(%rdx), %rbx\n"
        "    movq 8(%rdx), %rbp\n"
        "    movq 16(%rdx), %r12\n"
        "    movq 24(%rdx), %r13\n"
        "    movq 32(%rdx), %r14\n"
        "    movq 40(%rdx), %r15\n"
        "    call AsteriaTestAmd64SwitchContexts\n"
        "    movq %rax, %r10\n"
        "    movq 0(%rsp), %rax\n"
        "    movq %rbx, 0(%rax)\n"
        "    movq %rbp, 8(%rax)\n"
        "    movq %r12, 16(%rax)\n"
        "    movq %r13, 24(%rax)\n"
        "    movq %r14, 32(%rax)\n"
        "    movq %r15, 40(%rax)\n"
        "    addq $8, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    movq %r10, %rax\n"
        "    ret\n"
        ".size AsteriaTestAmd64SwitchWithSentinels, "
        ". - AsteriaTestAmd64SwitchWithSentinels\n"

        ".p2align 4\n"
        ".local AsteriaTestAmd64WorkerEntry\n"
        ".type AsteriaTestAmd64WorkerEntry, @function\n"
        "AsteriaTestAmd64WorkerEntry:\n"
        "    movq %rsp, AsteriaTestAmd64EntryStackPointer(%rip)\n"
        "    movq %rdi, AsteriaTestAmd64EntryArgument(%rip)\n"
        "    jmp AsteriaTestAmd64WorkerBody\n"
        ".size AsteriaTestAmd64WorkerEntry, "
        ". - AsteriaTestAmd64WorkerEntry\n");

namespace
{
alignas(4096) UInt8 g_workerStack[16U * 1024U]{};
Amd64KernelContext g_mainContext;
Amd64KernelContext g_workerContext;
int g_phase{0};
int g_failure{0};
constexpr UInt64 MainFirstSentinels[6]{
    0x1111'1111'1111'1111ULL, 0x2222'2222'2222'2222ULL, 0x3333'3333'3333'3333ULL,
    0x4444'4444'4444'4444ULL, 0x5555'5555'5555'5555ULL, 0x6666'6666'6666'6666ULL,
};
constexpr UInt64 MainSecondSentinels[6]{
    0xA1A1'A1A1'A1A1'A1A1ULL, 0xA2A2'A2A2'A2A2'A2A2ULL, 0xA3A3'A3A3'A3A3'A3A3ULL,
    0xA4A4'A4A4'A4A4'A4A4ULL, 0xA5A5'A5A5'A5A5'A5A5ULL, 0xA6A6'A6A6'A6A6'A6A6ULL,
};
constexpr UInt64 WorkerSentinels[6]{
    0xB1B1'B1B1'B1B1'B1B1ULL, 0xB2B2'B2B2'B2B2'B2B2ULL, 0xB3B3'B3B3'B3B3'B3B3ULL,
    0xB4B4'B4B4'B4B4'B4B4ULL, 0xB5B5'B5B5'B5B5'B5B5ULL, 0xB6B6'B6B6'B6B6'B6B6ULL,
};
UInt64 g_mainObserved[6]{};
UInt64 g_workerObserved[6]{};

[[nodiscard]] bool RegistersMatch(const UInt64* const observed, const UInt64* const expected) noexcept
{
    for (Asteria::Foundation::Size index{0U}; index < 6U; ++index)
    {
        if (observed[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] WritableAmd64KernelStackView WorkerStack() noexcept
{
    return WritableAmd64KernelStackView{
        .Data = g_workerStack,
        .SizeBytes = sizeof(g_workerStack),
        .VirtualStart = static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(g_workerStack)),
    };
}

void Worker(void* const argument) noexcept
{
    if ((argument != &g_phase) || (g_phase != 0))
    {
        g_failure = 1;
    }
    g_phase = 1;
    if (AsteriaTestAmd64SwitchWithSentinels(&g_workerContext, &g_mainContext, WorkerSentinels, g_workerObserved) !=
        static_cast<UInt64>(Amd64KernelContextError::None))
    {
        g_failure = 2;
    }
    if ((g_phase != 2) || !RegistersMatch(g_workerObserved, WorkerSentinels))
    {
        g_failure = 3;
    }
    g_phase = 3;
    if (AsteriaTestAmd64SwitchWithSentinels(&g_workerContext, &g_mainContext, WorkerSentinels, g_workerObserved) !=
        static_cast<UInt64>(Amd64KernelContextError::None))
    {
        g_failure = 4;
    }
    g_failure = 5;
    __builtin_trap();
}
} // Namespace

extern "C" void AsteriaTestAmd64WorkerBody(void* const argument) noexcept
{
    Worker(argument);
}

/// Returns zero, or the first failed stage of the native switch sequence:
/// 1 worker preparation, 2 first switch, 3 first-return state, 4 second
/// switch, 5 final state.
[[nodiscard]] static int RunNativeSwitches() noexcept
{
    const WritableAmd64KernelStackView workerStack = WorkerStack();
    if (g_workerContext.Prepare(workerStack, &AsteriaTestAmd64WorkerEntry, &g_phase) != Amd64KernelContextError::None)
    {
        return 1;
    }
    if (AsteriaTestAmd64SwitchWithSentinels(&g_mainContext, &g_workerContext, MainFirstSentinels, g_mainObserved) !=
        static_cast<UInt64>(Amd64KernelContextError::None))
    {
        return 2;
    }
    if ((g_phase != 1) || (g_failure != 0) || (AsteriaTestAmd64EntryArgument != &g_phase) ||
        (AsteriaTestAmd64EntryStackPointer != (workerStack.VirtualStart + workerStack.SizeBytes - 8U)) ||
        ((AsteriaTestAmd64EntryStackPointer & 15U) != 8U) || !RegistersMatch(g_mainObserved, MainFirstSentinels))
    {
        return 3;
    }
    g_phase = 2;
    if (AsteriaTestAmd64SwitchWithSentinels(&g_mainContext, &g_workerContext, MainSecondSentinels, g_mainObserved) !=
        static_cast<UInt64>(Amd64KernelContextError::None))
    {
        return 4;
    }
    return ((g_phase == 3) && (g_failure == 0) && RegistersMatch(g_mainObserved, MainSecondSentinels)) ? 0 : 5;
}

// Contexts with extended-state areas keep their own xmm registers and MXCSR across switches, and a
// new context starts from the initial state.
namespace
{
alignas(4096) UInt8 g_vectorStack[16U * 1024U]{};
Amd64KernelContext g_vectorMain;
Amd64KernelContext g_vectorWorker;
Amd64ExtendedStateArea g_vectorMainState{};
Amd64ExtendedStateArea g_vectorWorkerState{};
constexpr UInt64 MainVectors[4]{0x0123'4567'89AB'CDEFULL, 0x1111'2222'3333'4444ULL, 0x5555'6666'7777'8888ULL,
                                0x9999'AAAA'BBBB'CCCCULL};
constexpr UInt64 WorkerVectors[4]{0xFEDC'BA98'7654'3210ULL, 0xDDDD'EEEE'FFFF'0000ULL, 0x0F0F'0F0F'0F0F'0F0FULL,
                                  0xF0F0'F0F0'F0F0'F0F0ULL};
constexpr unsigned MainMxcsr{0x3F80U};
int g_vectorFailure{0};

void LoadVectors(const UInt64* const values) noexcept
{
    __asm__ __volatile__("movdqu (%0), %%xmm0\n\tmovdqu 16(%0), %%xmm15" : : "r"(values) : "memory");
}

void StoreVectors(UInt64* const values) noexcept
{
    __asm__ __volatile__("movdqu %%xmm0, (%0)\n\tmovdqu %%xmm15, 16(%0)" : : "r"(values) : "memory");
}

[[nodiscard]] unsigned ReadMxcsr() noexcept
{
    unsigned value{0U};
    __asm__ __volatile__("stmxcsr %0" : "=m"(value));
    return value;
}

void WriteMxcsr(unsigned value) noexcept
{
    __asm__ __volatile__("ldmxcsr %0" : : "m"(value));
}

[[nodiscard]] bool VectorsEqual(const UInt64* const observed, const UInt64* const expected) noexcept
{
    for (Asteria::Foundation::Size index{0U}; index < 4U; ++index)
    {
        if (observed[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

void VectorWorker(void*) noexcept
{
    UInt64 observed[4]{};
    StoreVectors(observed);
    constexpr UInt64 Zero[4]{};
    if ((ReadMxcsr() != 0x1F80U) || !VectorsEqual(observed, Zero))
    {
        g_vectorFailure = 1;
    }
    LoadVectors(WorkerVectors);
    if (g_vectorWorker.SwitchTo(g_vectorMain) != Amd64KernelContextError::None)
    {
        g_vectorFailure = 2;
    }
    StoreVectors(observed);
    if (!VectorsEqual(observed, WorkerVectors) || (ReadMxcsr() != 0x1F80U))
    {
        g_vectorFailure = 3;
    }
    static_cast<void>(g_vectorWorker.SwitchTo(g_vectorMain));
    g_vectorFailure = 4;
    __builtin_trap();
}

/// Returns zero, or the first failed stage: 1 the worker's initial state, 2 a
/// switch, 3 the worker's own vectors after a round trip, 5 the main
/// context's vectors or MXCSR after a round trip.
[[nodiscard]] int RunVectorSwitches() noexcept
{
    g_vectorMain.AttachExtendedState(g_vectorMainState);
    g_vectorWorker.AttachExtendedState(g_vectorWorkerState);
    if (g_vectorWorker.Prepare(
            WritableAmd64KernelStackView{
                .Data = g_vectorStack,
                .SizeBytes = sizeof(g_vectorStack),
                .VirtualStart = static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(g_vectorStack)),
            },
            &VectorWorker, nullptr) != Amd64KernelContextError::None)
    {
        return 2;
    }
    const unsigned saved = ReadMxcsr();
    LoadVectors(MainVectors);
    WriteMxcsr(MainMxcsr);
    if (g_vectorMain.SwitchTo(g_vectorWorker) != Amd64KernelContextError::None)
    {
        return 2;
    }
    UInt64 observed[4]{};
    StoreVectors(observed);
    const unsigned mxcsr = ReadMxcsr();
    if (g_vectorMain.SwitchTo(g_vectorWorker) != Amd64KernelContextError::None)
    {
        return 2;
    }
    WriteMxcsr(saved);
    if (g_vectorFailure != 0)
    {
        return g_vectorFailure;
    }
    return (VectorsEqual(observed, MainVectors) && (mxcsr == MainMxcsr)) ? 0 : 5;
}
} // Namespace

int main()
{
    TestContext runner{"Amd64KernelContextNativeTests"};
    ASTERIA_CHECK_EQ(runner, RunNativeSwitches(), 0);
    ASTERIA_CHECK_EQ(runner, RunVectorSwitches(), 0);
    return runner.Finish();
}
