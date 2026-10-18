// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Ia64/Memory/Ia64LocalTlbInvalidator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::UInt64 g_purgedAddresses[512]{};
Foundation::UInt64 g_purgeEncodings[512]{};
Foundation::Size g_purgeCount{0U};
Foundation::Size g_serializationCount{0U};
Foundation::Size g_purgesObservedAtSerialization{0U};
} // namespace Asteria::Test

extern "C" void AsteriaIa64PurgeLocalTranslation(const Asteria::Foundation::UInt64 virtualAddress,
                                                 const Asteria::Foundation::UInt64 encodedPageSize) noexcept
{
    if (Asteria::Test::g_purgeCount < 512U)
    {
        Asteria::Test::g_purgedAddresses[Asteria::Test::g_purgeCount] = virtualAddress;
        Asteria::Test::g_purgeEncodings[Asteria::Test::g_purgeCount] = encodedPageSize;
    }
    ++Asteria::Test::g_purgeCount;
}

extern "C" void AsteriaIa64SerializeLocalTranslationPurge() noexcept
{
    ++Asteria::Test::g_serializationCount;
    Asteria::Test::g_purgesObservedAtSerialization = Asteria::Test::g_purgeCount;
}

namespace Asteria::Test
{
using Hal::Memory::ILocalTlbInvalidator;
using Hal::Memory::LocalTlbInvalidationError;
using Kernel::Memory::PageCount;
using Kernel::Memory::VirtualAddress;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::Ia64LocalTlbInvalidator;
using Platform::Ia64::Memory::Ia64LocalTlbInvalidatorInitializationError;
using Platform::Ia64::Memory::Ia64LocalTlbInvalidatorState;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::TranslationCapabilities;

[[nodiscard]] constexpr PalVirtualMemoryCapabilities ValidCapabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 48U,
                .ImplementedVirtualAddressMostSignificantBit = 53U,
                .RegionIdentifierBits = 18U,
                .ProtectionKeyBits = 18U,
                .MemoryAttributeMask = 0x11U,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 8U,
        .DataTranslationRegisterCount = 8U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 0U,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

void ResetLog() noexcept
{
    for (Foundation::Size index{0U}; index < 512U; ++index)
    {
        g_purgedAddresses[index] = 0U;
        g_purgeEncodings[index] = 0U;
    }
    g_purgeCount = 0U;
    g_serializationCount = 0U;
    g_purgesObservedAtSerialization = 0U;
}

void TestInitialization(TestContext& runner) noexcept
{
    Ia64LocalTlbInvalidator invalidator;
    ResetLog();
    ASTERIA_EXPECT(runner, "an uninitialized invalidator performs no native operation",
                   invalidator.Invalidate(VirtualAddress{0x400000U}, PageCount{1U}) ==
                       LocalTlbInvalidationError::NotReady,
                   g_purgeCount == 0U, g_serializationCount == 0U);

    PalVirtualMemoryCapabilities invalidCapabilities = ValidCapabilities();
    invalidCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit = 49U;
    ASTERIA_EXPECT(runner, "an invalid virtual-address width leaves initialization retryable",
                   invalidator.Initialize(invalidCapabilities) ==
                       Ia64LocalTlbInvalidatorInitializationError::InvalidCapabilities,
                   invalidator.GetSnapshot().State == Ia64LocalTlbInvalidatorState::Uninitialized);

    invalidCapabilities = ValidCapabilities();
    invalidCapabilities.Translation.ImplementedVirtualAddressMostSignificantBit = 61U;
    ASTERIA_EXPECT(runner, "a virtual-address width beyond the PAL field range is rejected",
                   invalidator.Initialize(invalidCapabilities) ==
                       Ia64LocalTlbInvalidatorInitializationError::InvalidCapabilities,
                   invalidator.GetSnapshot().State == Ia64LocalTlbInvalidatorState::Uninitialized);

    invalidCapabilities = ValidCapabilities();
    invalidCapabilities.PurgePageSizeMask &= ~(Foundation::UInt64{1U} << 12U);
    ASTERIA_EXPECT(runner, "a processor without 4 KiB local purge support is rejected",
                   invalidator.Initialize(invalidCapabilities) ==
                       Ia64LocalTlbInvalidatorInitializationError::InvalidCapabilities);

    const PalVirtualMemoryCapabilities capabilities = ValidCapabilities();
    ASTERIA_EXPECT(runner, "decoded processor-local capabilities initialize exactly once",
                   invalidator.Initialize(capabilities) == Ia64LocalTlbInvalidatorInitializationError::None,
                   invalidator.GetSnapshot().State == Ia64LocalTlbInvalidatorState::Ready,
                   invalidator.GetSnapshot().ImplementedVirtualAddressMostSignificantBit == 53U);
    ASTERIA_EXPECT(runner, "a ready invalidator cannot change its retained CPU profile",
                   invalidator.Initialize(capabilities) ==
                       Ia64LocalTlbInvalidatorInitializationError::AlreadyInitialized);
}

struct InvalidRangeCase
{
    VirtualAddress Start;
    PageCount Pages;
    LocalTlbInvalidationError Error;
};

void TestValidation(TestContext& runner) noexcept
{
    Ia64LocalTlbInvalidator invalidator;
    static_cast<void>(invalidator.Initialize(ValidCapabilities()));
    constexpr InvalidRangeCase Cases[]{
        {VirtualAddress{0x004001U}, PageCount{1U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0x004000U}, PageCount{0U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0x004000U}, PageCount{513U}, LocalTlbInvalidationError::RangeTooLarge},
        {VirtualAddress{0xFFFF'FFFF'FFFF'F000ULL}, PageCount{2U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0xE040'0000'0000'0000ULL}, PageCount{1U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0xE01F'FFFF'FFFF'F000ULL}, PageCount{2U}, LocalTlbInvalidationError::InvalidRange},
        {VirtualAddress{0xFFDF'FFFF'FFFF'F000ULL}, PageCount{2U}, LocalTlbInvalidationError::InvalidRange},
    };

    for (const InvalidRangeCase& test : Cases)
    {
        ResetLog();
        ASTERIA_EXPECT(runner, "an invalid IA-64 purge range performs no native operation",
                       invalidator.Invalidate(test.Start, test.Pages) == test.Error, g_purgeCount == 0U,
                       g_serializationCount == 0U);
    }
}

void TestExactInvalidations(TestContext& runner) noexcept
{
    Ia64LocalTlbInvalidator invalidator;
    ILocalTlbInvalidator& interface = invalidator;
    static_cast<void>(invalidator.Initialize(ValidCapabilities()));

    ResetLog();
    ASTERIA_EXPECT(runner, "each page is purged before one final instruction serialization",
                   interface.Invalidate(VirtualAddress{0xE000'0000'0040'0000ULL}, PageCount{3U}) ==
                       LocalTlbInvalidationError::None,
                   g_purgeCount == 3U, g_purgedAddresses[0U] == 0xE000'0000'0040'0000ULL,
                   g_purgedAddresses[1U] == 0xE000'0000'0040'1000ULL, g_purgedAddresses[2U] == 0xE000'0000'0040'2000ULL,
                   g_purgeEncodings[0U] == 0x30U, g_purgeEncodings[1U] == 0x30U, g_purgeEncodings[2U] == 0x30U,
                   g_serializationCount == 1U, g_purgesObservedAtSerialization == 3U);

    ResetLog();
    ASTERIA_EXPECT(runner, "the maximum sign-extended IA-64 range remains bounded and complete",
                   invalidator.Invalidate(VirtualAddress{0xFFE0'0000'0040'0000ULL}, PageCount{512U}) ==
                       LocalTlbInvalidationError::None,
                   g_purgeCount == 512U, g_purgedAddresses[0U] == 0xFFE0'0000'0040'0000ULL,
                   g_purgedAddresses[511U] == 0xFFE0'0000'005F'F000ULL, g_serializationCount == 1U,
                   g_purgesObservedAtSerialization == 512U);

    ResetLog();
    ASTERIA_EXPECT(runner, "the final region-7 page is purged and then serialized",
                   invalidator.Invalidate(VirtualAddress{0xFFFF'FFFF'FFFF'F000ULL}, PageCount{1U}) ==
                       LocalTlbInvalidationError::None,
                   g_purgeCount == 1U, g_purgedAddresses[0U] == 0xFFFF'FFFF'FFFF'F000ULL, g_purgeEncodings[0U] == 0x30U,
                   g_serializationCount == 1U, g_purgesObservedAtSerialization == 1U);

    ResetLog();
    ASTERIA_EXPECT(runner, "a multi-page region-7 range may end exactly at UINT64_MAX",
                   invalidator.Invalidate(VirtualAddress{0xFFFF'FFFF'FFFF'E000ULL}, PageCount{2U}) ==
                       LocalTlbInvalidationError::None,
                   g_purgeCount == 2U, g_purgedAddresses[0U] == 0xFFFF'FFFF'FFFF'E000ULL,
                   g_purgedAddresses[1U] == 0xFFFF'FFFF'FFFF'F000ULL, g_purgeEncodings[0U] == 0x30U,
                   g_purgeEncodings[1U] == 0x30U, g_serializationCount == 1U, g_purgesObservedAtSerialization == 2U);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64LocalTlbInvalidatorTests"};
    Asteria::Test::TestInitialization(runner);
    Asteria::Test::TestValidation(runner);
    Asteria::Test::TestExactInvalidations(runner);
    return runner.Finish();
}
