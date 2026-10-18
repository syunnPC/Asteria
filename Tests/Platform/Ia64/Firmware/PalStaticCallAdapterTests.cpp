// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Platform/Ia64/Firmware/PalStaticCallAdapter.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Platform::Ia64::Firmware::PalStaticCallAdapter;
using Platform::Ia64::Firmware::PalStaticCallAdapterError;
using Platform::Ia64::Firmware::SalEntrypointBinding;
using Platform::Ia64::Memory::PalStaticCallResult;

struct InvocationRecord
{
    Foundation::UInt64 EntryPhysicalAddress;
    Foundation::UInt64 ProcedureIndex;
    Foundation::UInt64 Argument1;
    Foundation::UInt64 Argument2;
    Foundation::UInt64 Argument3;
    Foundation::UInt64 InvocationCount;
};

constinit InvocationRecord g_invocation{};
constinit Foundation::UInt64 g_stubInvocationState;
constinit PalStaticCallResult g_stubResult{};
Foundation::UInt8 g_bindingSnapshot[sizeof(SalEntrypointBinding)];

[[nodiscard]] constexpr PhysicalMemoryRegion Region(const Foundation::UInt64 physicalStart,
                                                    const Foundation::UInt64 virtualStart,
                                                    const Foundation::UInt64 pageCount,
                                                    const Foundation::UInt32 firmwareType,
                                                    const PhysicalMemoryKind kind) noexcept
{
    return PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = virtualStart,
        .PageCount = pageCount,
        .Attributes = Foundation::UInt64{1U} << 3U,
        .FirmwareType = firmwareType,
        .Kind = kind,
    };
}

[[nodiscard]] constexpr SalEntrypointBinding ValidBinding() noexcept
{
    return SalEntrypointBinding{
        .PalProcedurePhysicalAddress = 0x2000U,
        .SalProcedurePhysicalAddress = 0x4000U,
        .SalGlobalDataPhysicalAddress = 0x6000U,
        .PalCodeRegion = Region(0x1000U, 0U, 2U, 13U, PhysicalMemoryKind::PalCode),
        .SalCodeRegion = Region(0x4000U, 0U, 2U, 5U, PhysicalMemoryKind::RuntimeServices),
        .SalGlobalDataRegion = Region(0x6000U, 0U, 2U, 6U, PhysicalMemoryKind::RuntimeServices),
    };
}

void ResetStub() noexcept
{
    g_invocation = InvocationRecord{};
    g_stubInvocationState = 0U;
    g_stubResult = PalStaticCallResult{
        .Status = -7,
        .Result1 = 0x1111'2222'3333'4444U,
        .Result2 = 0x5555'6666'7777'8888U,
        .Result3 = 0x9999'AAAA'BBBB'CCCCU,
    };
}

[[nodiscard]] bool IsRecordedCall(const Foundation::UInt64 expectedIndex,
                                  const Foundation::UInt64 expectedEntry) noexcept
{
    return (g_invocation.InvocationCount == 1U) && (g_invocation.EntryPhysicalAddress == expectedEntry) &&
           (g_invocation.ProcedureIndex == expectedIndex) && (g_invocation.Argument1 == 0U) &&
           (g_invocation.Argument2 == 0U) && (g_invocation.Argument3 == 0U);
}

[[nodiscard]] bool
IsStubResult(const Foundation::Result<PalStaticCallResult, PalStaticCallAdapterError>& result) noexcept
{
    if (!result.HasValue())
    {
        return false;
    }
    const PalStaticCallResult& value = result.GetValue();
    return (value.Status == g_stubResult.Status) && (value.Result1 == g_stubResult.Result1) &&
           (value.Result2 == g_stubResult.Result2) && (value.Result3 == g_stubResult.Result3);
}

void SnapshotBinding(const SalEntrypointBinding& binding) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&binding);
    for (Foundation::Size index{0U}; index < sizeof(binding); ++index)
    {
        g_bindingSnapshot[index] = bytes[index];
    }
}

[[nodiscard]] bool BindingMatchesSnapshot(const SalEntrypointBinding& binding) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&binding);
    for (Foundation::Size index{0U}; index < sizeof(binding); ++index)
    {
        if (g_bindingSnapshot[index] != bytes[index])
        {
            return false;
        }
    }
    return true;
}

template <typename TInvoker>
void CheckFixedProcedure(TestContext& runner, const char* const description, const Foundation::UInt64 expectedIndex,
                         TInvoker invoker) noexcept
{
    ResetStub();
    const SalEntrypointBinding binding = ValidBinding();
    const auto result = invoker(binding);
    ASTERIA_EXPECT(runner, description, IsStubResult(result),
                   IsRecordedCall(expectedIndex, binding.PalProcedurePhysicalAddress));
}

[[nodiscard]] bool HasInvalidBindingError(const SalEntrypointBinding& binding) noexcept
{
    ResetStub();
    const auto result = PalStaticCallAdapter::QueryMemoryAttributes(binding);
    return !result.HasValue() && (result.GetError() == PalStaticCallAdapterError::InvalidBinding) &&
           (g_invocation.InvocationCount == 0U);
}

void TestFixedProcedures(TestContext& runner) noexcept
{
    CheckFixedProcedure(runner, "PAL_MEM_ATTRIB uses fixed index five and zero reserved arguments", 5U,
                        [](const SalEntrypointBinding& binding) noexcept
                        {
                            return PalStaticCallAdapter::QueryMemoryAttributes(binding);
                        });
    CheckFixedProcedure(runner, "PAL_PTCE_INFO uses fixed index six and zero reserved arguments", 6U,
                        [](const SalEntrypointBinding& binding) noexcept
                        {
                            return PalStaticCallAdapter::QueryTranslationCachePurgeInformation(binding);
                        });
    CheckFixedProcedure(runner, "PAL_VM_SUMMARY uses fixed index eight and zero reserved arguments", 8U,
                        [](const SalEntrypointBinding& binding) noexcept
                        {
                            return PalStaticCallAdapter::QueryVirtualMemorySummary(binding);
                        });
    CheckFixedProcedure(runner, "PAL_VM_PAGE_SIZE uses fixed index 34 and zero reserved arguments", 34U,
                        [](const SalEntrypointBinding& binding) noexcept
                        {
                            return PalStaticCallAdapter::QueryVirtualMemoryPageSizes(binding);
                        });
    CheckFixedProcedure(runner, "PAL_RSE_INFO uses fixed index 19 and zero reserved arguments", 19U,
                        [](const SalEntrypointBinding& binding) noexcept
                        {
                            return PalStaticCallAdapter::QueryRseInformation(binding);
                        });
}

void TestInvalidBindings(TestContext& runner) noexcept
{
    SalEntrypointBinding binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0U;
    ASTERIA_EXPECT(runner, "zero PAL entry is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0x2008U;
    ASTERIA_EXPECT(runner, "non-bundle-aligned PAL entry is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0x7FFF'FFFF'FFFF'FFFFU;
    ASTERIA_EXPECT(runner, "PAL bundle crossing the 63-bit physical limit is rejected",
                   HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0x0FF0U;
    ASTERIA_EXPECT(runner, "PAL entry before its retained region is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0x3000U;
    ASTERIA_EXPECT(runner, "PAL entry after its retained region is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.PageCount = 0U;
    ASTERIA_EXPECT(runner, "empty PAL-code region is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.PageCount = (Foundation::UInt64{1U} << 52U) + 1U;
    ASTERIA_EXPECT(runner, "oversized PAL-code region is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.PhysicalStart = 0x1001U;
    ASTERIA_EXPECT(runner, "unaligned PAL-code physical start is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.VirtualStart = 1U;
    ASTERIA_EXPECT(runner, "unaligned retained virtual start is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.PageCount = Foundation::UInt64{1U} << 52U;
    ASTERIA_EXPECT(runner, "full-address-space page count with nonzero start is rejected",
                   HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.PhysicalStart = 0xFFFF'FFFF'FFFF'F000U;
    binding.PalCodeRegion.PageCount = 2U;
    ASTERIA_EXPECT(runner, "overflowing PAL-code extent is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.FirmwareType = 5U;
    ASTERIA_EXPECT(runner, "non-EfiPalCode firmware type is rejected", HasInvalidBindingError(binding));

    binding = ValidBinding();
    binding.PalCodeRegion.Kind = PhysicalMemoryKind::RuntimeServices;
    ASTERIA_EXPECT(runner, "non-PAL normalized memory kind is rejected", HasInvalidBindingError(binding));
}

void TestPhysicalLimitAndFailureState(TestContext& runner) noexcept
{
    SalEntrypointBinding binding = ValidBinding();
    binding.PalProcedurePhysicalAddress = 0x7FFF'FFFF'FFFF'FFF0U;
    binding.PalCodeRegion = Region(0x7FFF'FFFF'FFFF'E000U, 0U, 2U, 13U, PhysicalMemoryKind::PalCode);
    ResetStub();
    const auto boundaryResult = PalStaticCallAdapter::QueryMemoryAttributes(binding);
    ASTERIA_EXPECT(runner, "last complete 63-bit PAL bundle is accepted", IsStubResult(boundaryResult),
                   IsRecordedCall(5U, 0x7FFF'FFFF'FFFF'FFF0U));

    binding = ValidBinding();
    SnapshotBinding(binding);
    ResetStub();
    g_stubInvocationState = 1U;
    const auto stateFailure = PalStaticCallAdapter::QueryVirtualMemorySummary(binding);
    ASTERIA_EXPECT(runner, "assembly mode rejection becomes a typed adapter failure", !stateFailure.HasValue(),
                   stateFailure.GetError() == PalStaticCallAdapterError::UnsupportedProcessorState,
                   IsRecordedCall(8U, binding.PalProcedurePhysicalAddress));
    ASTERIA_EXPECT(runner, "successful and failed adapter calls do not mutate the binding",
                   BindingMatchesSnapshot(binding));
}
} // namespace
} // namespace Asteria::Test

extern "C" Asteria::Platform::Ia64::Memory::PalStaticCallResult AsteriaIa64InvokePhysicalPalStatic(
    const Asteria::Foundation::UInt64 entryPhysicalAddress, const Asteria::Foundation::UInt64 procedureIndex,
    const Asteria::Foundation::UInt64 argument1, const Asteria::Foundation::UInt64 argument2,
    const Asteria::Foundation::UInt64 argument3, Asteria::Foundation::UInt64* const invocationState) noexcept
{
    ++Asteria::Test::g_invocation.InvocationCount;
    Asteria::Test::g_invocation.EntryPhysicalAddress = entryPhysicalAddress;
    Asteria::Test::g_invocation.ProcedureIndex = procedureIndex;
    Asteria::Test::g_invocation.Argument1 = argument1;
    Asteria::Test::g_invocation.Argument2 = argument2;
    Asteria::Test::g_invocation.Argument3 = argument3;
    *invocationState = Asteria::Test::g_stubInvocationState;
    return Asteria::Test::g_stubResult;
}

int main()
{
    TestContext runner{"PalStaticCallAdapterTests"};
    Asteria::Test::TestFixedProcedures(runner);
    Asteria::Test::TestInvalidBindings(runner);
    Asteria::Test::TestPhysicalLimitAndFailureState(runner);
    return runner.Finish();
}
