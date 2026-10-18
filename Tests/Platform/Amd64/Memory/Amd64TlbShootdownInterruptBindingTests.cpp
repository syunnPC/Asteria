// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Amd64/Memory/Amd64TlbShootdownInterruptBinding.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::UInt64 g_currentX2ApicId{0U};
Foundation::UInt64 g_events[16]{};
Foundation::Size g_eventCount{0U};
Foundation::Size g_eoiCount{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadX2ApicIdentifier() noexcept
{
    Asteria::Test::g_events[Asteria::Test::g_eventCount] = 0x10U;
    ++Asteria::Test::g_eventCount;
    return Asteria::Test::g_currentX2ApicId;
}

extern "C" void AsteriaAmd64WriteX2ApicEndOfInterrupt() noexcept
{
    Asteria::Test::g_events[Asteria::Test::g_eventCount] = 0xE0U;
    ++Asteria::Test::g_eventCount;
    ++Asteria::Test::g_eoiCount;
}

extern "C" void AsteriaAmd64WriteX2ApicInterruptCommand(Asteria::Foundation::UInt64) noexcept
{
}

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::ITlbShootdownDrain;
using Kernel::Memory::TlbShootdownDrainSummary;
using Kernel::Memory::TlbShootdownError;
using Kernel::Memory::TlbShootdownReceiveError;
using Kernel::Scheduler::CpuId;
using Platform::Amd64::Memory::Amd64TlbShootdownInterruptBinding;
using Platform::Amd64::Memory::Amd64TlbShootdownInterruptBindingInitializationError;
using Platform::Amd64::Memory::Amd64TlbShootdownInterruptBindingState;
using Platform::Amd64::Memory::Amd64TlbShootdownInterruptError;
using Platform::Amd64::Memory::Amd64X2ApicTlbShootdownNotifier;
using Platform::Amd64::Memory::Amd64X2ApicTlbShootdownNotifierInitializationError;

[[nodiscard]] constexpr TlbShootdownDrainSummary SummaryFor(const CpuId processor) noexcept
{
    return TlbShootdownDrainSummary{
        .ReceivedCount = 1U,
        .CompletedCount = 1U,
        .RejectedCount = 0U,
        .ReceiveError = TlbShootdownReceiveError::None,
        .FirstRemoteError = TlbShootdownError::None,
        .FirstLocalError = Hal::Memory::LocalTlbInvalidationError::None,
        .FirstAddressSpaceError = AddressSpaceError::None,
        .Processor = processor,
        .Reserved = {},
    };
}

class TestDrain final : public ITlbShootdownDrain
{
public:
    explicit constexpr TestDrain(const CpuId processor) noexcept : m_processor{processor}
    {
    }

    [[nodiscard]] TlbShootdownDrainSummary Drain() noexcept override
    {
        g_events[g_eventCount] = static_cast<Foundation::UInt64>(0x20U + m_processor.GetValue());
        ++g_eventCount;
        ++Calls;
        return SummaryFor(m_processor);
    }

    [[nodiscard]] CpuId GetProcessor() const noexcept override
    {
        return m_processor;
    }

    Foundation::Size Calls{0U};

private:
    CpuId m_processor;
};

void ResetEvents() noexcept
{
    for (Foundation::UInt64& event : g_events)
    {
        event = 0U;
    }
    g_eventCount = 0U;
    g_eoiCount = 0U;
}

void TestBinding(TestContext& runner) noexcept
{
    constexpr Foundation::UInt32 X2ApicIds[]{
        0x101U,
        0x202U,
        0x303U,
        0x404U,
    };
    constexpr Foundation::UInt8 OnlineMask{0x0BU};
    constexpr Foundation::UInt8 Vector{0xF0U};

    Amd64TlbShootdownInterruptBinding binding;
    g_currentX2ApicId = 0x101U;
    ResetEvents();
    AsteriaAmd64TlbShootdownInterruptEntry();
    ASTERIA_EXPECT(runner, "an interrupt before publication is acknowledged without dereference", g_eoiCount == 1U,
                   g_eventCount == 1U, g_events[0U] == 0xE0U);

    Amd64X2ApicTlbShootdownNotifier notifier;
    ITlbShootdownDrain* noDrains[Kernel::Scheduler::MaximumProcessorCount]{};
    ASTERIA_EXPECT(runner, "an unready notifier cannot publish an interrupt binding",
                   binding.Initialize(notifier, noDrains) ==
                       Amd64TlbShootdownInterruptBindingInitializationError::NotifierNotReady,
                   binding.GetSnapshot().State == Amd64TlbShootdownInterruptBindingState::Uninitialized);
    ASTERIA_EXPECT(runner, "the interrupt binding consumes one ready notifier topology",
                   notifier.Initialize(Vector, OnlineMask, X2ApicIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::None);

    TestDrain processorZero{CpuId{0U}};
    TestDrain processorOne{CpuId{1U}};
    TestDrain processorTwo{CpuId{2U}};
    TestDrain processorThree{CpuId{3U}};
    ITlbShootdownDrain* missingDrain[]{
        &processorZero,
        nullptr,
        nullptr,
        &processorThree,
    };
    ASTERIA_EXPECT(runner, "every online processor requires its exact bound drain",
                   binding.Initialize(notifier, missingDrain) ==
                       Amd64TlbShootdownInterruptBindingInitializationError::InvalidDrain,
                   binding.GetSnapshot().State == Amd64TlbShootdownInterruptBindingState::Uninitialized);

    ITlbShootdownDrain* offlineDrain[]{
        &processorZero,
        &processorOne,
        &processorTwo,
        &processorThree,
    };
    ASTERIA_EXPECT(runner, "offline processors cannot retain a hidden interrupt drain",
                   binding.Initialize(notifier, offlineDrain) ==
                       Amd64TlbShootdownInterruptBindingInitializationError::InvalidDrain,
                   binding.GetSnapshot().State == Amd64TlbShootdownInterruptBindingState::Uninitialized);

    ITlbShootdownDrain* drains[]{
        &processorZero,
        &processorOne,
        nullptr,
        &processorThree,
    };
    ASTERIA_EXPECT(runner, "one exact per-CPU drain topology is release-published",
                   binding.Initialize(notifier, drains) == Amd64TlbShootdownInterruptBindingInitializationError::None);
    const auto snapshot = binding.GetSnapshot();
    ASTERIA_EXPECT(runner, "the published binding retains the notifier topology exactly",
                   snapshot.X2ApicIds[0U] == 0x101U, snapshot.X2ApicIds[1U] == 0x202U, snapshot.X2ApicIds[2U] == 0U,
                   snapshot.X2ApicIds[3U] == 0x404U, snapshot.OnlineProcessorMask == OnlineMask,
                   snapshot.Vector == Vector, snapshot.State == Amd64TlbShootdownInterruptBindingState::Ready,
                   snapshot.Reserved == 0U);

    g_currentX2ApicId = 0x202U;
    ResetEvents();
    const auto processorOneResult = binding.HandleInterrupt();
    ASTERIA_EXPECT(runner, "the current x2APIC ID selects its drain before one EOI",
                   processorOneResult.Error == Amd64TlbShootdownInterruptError::None,
                   processorOneResult.ObservedX2ApicId == 0x202U, processorOneResult.Drain.Processor.GetValue() == 1U,
                   processorOneResult.Drain.ReceivedCount == 1U, processorOne.Calls == 1U, g_eoiCount == 1U,
                   g_eventCount == 3U, g_events[0U] == 0x10U, g_events[1U] == 0x21U, g_events[2U] == 0xE0U);

    g_currentX2ApicId = 0x999U;
    ResetEvents();
    const auto unknown = binding.HandleInterrupt();
    ASTERIA_EXPECT(runner, "an unknown hardware identity cannot run another CPU's drain",
                   unknown.Error == Amd64TlbShootdownInterruptError::InvalidProcessor,
                   unknown.ObservedX2ApicId == 0x999U, unknown.Drain.Processor.GetValue() == 0xFFU,
                   processorZero.Calls == 0U, processorOne.Calls == 1U, processorThree.Calls == 0U, g_eoiCount == 1U);

    g_currentX2ApicId = 0x1'0000'0000ULL;
    ResetEvents();
    const auto malformed = binding.HandleInterrupt();
    ASTERIA_EXPECT(runner, "reserved high x2APIC-ID bits are rejected before drain selection",
                   malformed.Error == Amd64TlbShootdownInterruptError::InvalidProcessor,
                   malformed.ObservedX2ApicId == 0x1'0000'0000ULL, g_eoiCount == 1U);

    g_currentX2ApicId = 0x404U;
    ResetEvents();
    AsteriaAmd64TlbShootdownInterruptEntry();
    ASTERIA_EXPECT(runner, "the installed host entry reaches the globally published binding",
                   processorThree.Calls == 1U, g_eoiCount == 1U, g_eventCount == 3U, g_events[0U] == 0x10U,
                   g_events[1U] == 0x23U, g_events[2U] == 0xE0U);

    ASTERIA_EXPECT(runner, "the installed binding is immutable and single-use",
                   binding.Initialize(notifier, drains) ==
                       Amd64TlbShootdownInterruptBindingInitializationError::AlreadyInitialized);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64TlbShootdownInterruptBindingTests"};
    Asteria::Test::TestBinding(runner);
    return runner.Finish();
}
