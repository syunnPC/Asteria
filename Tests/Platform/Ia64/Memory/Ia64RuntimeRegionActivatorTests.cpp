// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionActivator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_activationCallCount{0U};
Foundation::UInt64 g_forwardedRegionRegisters[7U]{};
Foundation::UInt64 g_adapterStatus{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateRuntimeRegions(
    const Asteria::Foundation::UInt64 regionRegister1, const Asteria::Foundation::UInt64 regionRegister2,
    const Asteria::Foundation::UInt64 regionRegister3, const Asteria::Foundation::UInt64 regionRegister4,
    const Asteria::Foundation::UInt64 regionRegister5, const Asteria::Foundation::UInt64 regionRegister6,
    const Asteria::Foundation::UInt64 regionRegister7) noexcept
{
    ++Asteria::Test::g_activationCallCount;
    Asteria::Test::g_forwardedRegionRegisters[0U] = regionRegister1;
    Asteria::Test::g_forwardedRegionRegisters[1U] = regionRegister2;
    Asteria::Test::g_forwardedRegionRegisters[2U] = regionRegister3;
    Asteria::Test::g_forwardedRegionRegisters[3U] = regionRegister4;
    Asteria::Test::g_forwardedRegionRegisters[4U] = regionRegister5;
    Asteria::Test::g_forwardedRegionRegisters[5U] = regionRegister6;
    Asteria::Test::g_forwardedRegionRegisters[6U] = regionRegister7;
    return Asteria::Test::g_adapterStatus;
}

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::Ia64BootstrapPreferredPageShift;
using Platform::Ia64::Memory::Ia64BootstrapRegionRegisterWords;
using Platform::Ia64::Memory::Ia64RuntimeRegionActivationError;
using Platform::Ia64::Memory::Ia64RuntimeRegionActivator;
using Platform::Ia64::Memory::Ia64RuntimeRegionCount;
using Platform::Ia64::Memory::Ia64RuntimeRegionSet;
using Platform::Ia64::Memory::Ia64RuntimeRegionSetError;
using Platform::Ia64::Memory::Ia64SharedBootstrapRegionIdentifier;
using Platform::Ia64::Memory::TranslationCapabilities;

constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterRejectedBeforeCommit{1U};
constexpr Foundation::UInt64 AdapterCommittedStateUnknown{2U};

[[nodiscard]] constexpr TranslationCapabilities Capabilities() noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = 0U,
        .PhysicalAddressBits = 0U,
        .ImplementedVirtualAddressMostSignificantBit = 0U,
        .RegionIdentifierBits = 24U,
        .ProtectionKeyBits = 0U,
        .MemoryAttributeMask = 0U,
        .HardwareWalkerImplemented = false,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr Foundation::UInt64 RegionRegisterWord(const Foundation::UInt32 regionIdentifier) noexcept
{
    return (static_cast<Foundation::UInt64>(regionIdentifier) << 8U) |
           (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U);
}

[[nodiscard]] constexpr Ia64BootstrapRegionRegisterWords BootstrapWords() noexcept
{
    Ia64BootstrapRegionRegisterWords words{};
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        words.RegionRegisters[index] =
            RegionRegisterWord(Ia64SharedBootstrapRegionIdentifier + static_cast<Foundation::UInt32>(index));
    }
    return words;
}

[[nodiscard]] bool BuildRegionSet(Ia64RuntimeRegionSet& output) noexcept
{
    return Ia64RuntimeRegionSet::AdoptBootstrap(BootstrapWords(), Capabilities(), output) ==
           Ia64RuntimeRegionSetError::None;
}

void ReplaceRegionRegister(Ia64RuntimeRegionSet& regionSet, const Foundation::Size regionIndex,
                           const Foundation::UInt64 regionRegister) noexcept
{
    static_assert(__is_standard_layout(Ia64RuntimeRegionSet));
    static_assert(__is_trivially_copyable(Ia64RuntimeRegionSet));
    Foundation::UInt8* const bytes = reinterpret_cast<Foundation::UInt8*>(&regionSet);
    static_cast<void>(
        std::memcpy(bytes + (regionIndex * sizeof(Foundation::UInt64)), &regionRegister, sizeof(regionRegister)));
}

void ResetAdapter(const Foundation::UInt64 status) noexcept
{
    g_activationCallCount = 0U;
    g_adapterStatus = status;
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        g_forwardedRegionRegisters[index] = 0U;
    }
}

[[nodiscard]] bool WasNotCalled() noexcept
{
    if (g_activationCallCount != 0U)
    {
        return false;
    }
    for (const Foundation::UInt64 regionRegister : g_forwardedRegionRegisters)
    {
        if (regionRegister != 0U)
        {
            return false;
        }
    }
    return true;
}

void CheckInvalid(TestContext& runner, Ia64RuntimeRegionSet& regionSet, const char* const description) noexcept
{
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, description,
                   Ia64RuntimeRegionActivator::Activate(regionSet) ==
                       Ia64RuntimeRegionActivationError::InvalidRegionSet,
                   WasNotCalled());
}

void CheckInvalidPhysicalBootstrap(TestContext& runner, Ia64RuntimeRegionSet& regionSet,
                                   const char* const description) noexcept
{
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, description,
                   Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet) ==
                       Ia64RuntimeRegionActivationError::InvalidRegionSet,
                   WasNotCalled());
}

void TestUnbuiltAndAnchorValidation(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet unbuilt;
    CheckInvalid(runner, unbuilt, "an unbuilt set is rejected before the native helper");
    CheckInvalidPhysicalBootstrap(runner, unbuilt,
                                  "the physical bootstrap entry rejects an unbuilt set before native state");

    Ia64RuntimeRegionSet invalidAnchor;
    const bool built = BuildRegionSet(invalidAnchor);
    ReplaceRegionRegister(invalidAnchor, 0U, RegionRegisterWord(0x2001U));
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "RR0 is validated before any private region can reach the helper", built,
                   Ia64RuntimeRegionActivator::Activate(invalidAnchor) ==
                       Ia64RuntimeRegionActivationError::InvalidRegionSet,
                   WasNotCalled());
    CheckInvalidPhysicalBootstrap(runner, invalidAnchor,
                                  "physical bootstrap validates the fixed RR0 anchor before native state");
}

void TestPrivateWordValidation(TestContext& runner) noexcept
{
    struct InvalidWordCase
    {
        Foundation::UInt64 Word;
        const char* Description;
    };

    constexpr InvalidWordCase cases[]{
        InvalidWordCase{
            .Word = RegionRegisterWord(0x2001U) | 1U,
            .Description = "RR1 with VHPT walking enabled is rejected before commit",
        },
        InvalidWordCase{
            .Word = RegionRegisterWord(0x2001U) | 2U,
            .Description = "RR1 with its reserved low bit set is rejected before commit",
        },
        InvalidWordCase{
            .Word = (Foundation::UInt64{0x2001U} << 8U) | (13U << 2U),
            .Description = "RR1 with a non-4-KiB preferred page size is rejected",
        },
        InvalidWordCase{
            .Word = RegionRegisterWord(0x2001U) | (Foundation::UInt64{1U} << 32U),
            .Description = "RR1 with a reserved high bit set is rejected before commit",
        },
        InvalidWordCase{
            .Word = RegionRegisterWord(0U),
            .Description = "RR1 with RID zero is rejected before commit",
        },
        InvalidWordCase{
            .Word = RegionRegisterWord(Ia64SharedBootstrapRegionIdentifier),
            .Description = "RR1 cannot reuse the fixed shared RR0 RID",
        },
    };

    for (const InvalidWordCase& invalid : cases)
    {
        Ia64RuntimeRegionSet regionSet;
        const bool built = BuildRegionSet(regionSet);
        ReplaceRegionRegister(regionSet, 1U, invalid.Word);
        ResetAdapter(AdapterSuccess);
        ASTERIA_EXPECT(runner, invalid.Description, built,
                       Ia64RuntimeRegionActivator::Activate(regionSet) ==
                           Ia64RuntimeRegionActivationError::InvalidRegionSet,
                       WasNotCalled());
    }

    Ia64RuntimeRegionSet duplicate;
    const bool duplicateBuilt = BuildRegionSet(duplicate);
    ReplaceRegionRegister(duplicate, 7U, RegionRegisterWord(0x2001U));
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "the last private RR is checked for duplicate RIDs before helper entry", duplicateBuilt,
                   Ia64RuntimeRegionActivator::Activate(duplicate) ==
                       Ia64RuntimeRegionActivationError::InvalidRegionSet,
                   WasNotCalled());
}

void TestExactForwardingAndSuccess(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet regionSet;
    const bool built = BuildRegionSet(regionSet);
    const Ia64BootstrapRegionRegisterWords expected{BootstrapWords()};
    ResetAdapter(AdapterSuccess);
    const Ia64RuntimeRegionActivationError error = Ia64RuntimeRegionActivator::Activate(regionSet);
    bool exactForwarding{g_activationCallCount == 1U};
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        exactForwarding =
            exactForwarding && (g_forwardedRegionRegisters[index] == expected.RegionRegisters[index + 1U]);
    }
    ASTERIA_EXPECT(runner, "one valid set forwards exactly RR1 through RR7 and reports success", built,
                   error == Ia64RuntimeRegionActivationError::None, exactForwarding);
}

void TestMinimumRepresentablePrivateRid(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet regionSet;
    const bool built = BuildRegionSet(regionSet);
    constexpr Foundation::UInt64 minimumPrivateWord{RegionRegisterWord(1U)};
    ReplaceRegionRegister(regionSet, 1U, minimumPrivateWord);
    ResetAdapter(AdapterSuccess);
    const Ia64RuntimeRegionActivationError error = Ia64RuntimeRegionActivator::Activate(regionSet);
    ASTERIA_EXPECT(runner, "the minimum architecturally valid private RID is forwarded exactly", built,
                   error == Ia64RuntimeRegionActivationError::None, g_activationCallCount == 1U,
                   g_forwardedRegionRegisters[0U] == minimumPrivateWord);
}

void TestMaximumRepresentablePrivateRid(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet regionSet;
    const bool built = BuildRegionSet(regionSet);
    constexpr Foundation::UInt64 maximumPrivateWord{RegionRegisterWord(0x00FF'FFFFU)};
    ReplaceRegionRegister(regionSet, 7U, maximumPrivateWord);
    ResetAdapter(AdapterSuccess);
    const Ia64RuntimeRegionActivationError error = Ia64RuntimeRegionActivator::Activate(regionSet);
    ASTERIA_EXPECT(runner, "the maximum architecturally representable private RID is forwarded exactly", built,
                   error == Ia64RuntimeRegionActivationError::None, g_activationCallCount == 1U,
                   g_forwardedRegionRegisters[6U] == maximumPrivateWord);
}

void TestNativeStatusMapping(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet regionSet;
    const bool built = BuildRegionSet(regionSet);

    ResetAdapter(AdapterRejectedBeforeCommit);
    const Ia64RuntimeRegionActivationError rejected = Ia64RuntimeRegionActivator::Activate(regionSet);
    ASTERIA_EXPECT(runner, "native precommit rejection remains distinguishable", built,
                   rejected == Ia64RuntimeRegionActivationError::RejectedBeforeCommit, g_activationCallCount == 1U);

    ResetAdapter(AdapterCommittedStateUnknown);
    const Ia64RuntimeRegionActivationError uncertain = Ia64RuntimeRegionActivator::Activate(regionSet);
    ASTERIA_EXPECT(runner, "postwrite readback uncertainty cannot be reported as success",
                   uncertain == Ia64RuntimeRegionActivationError::CommittedStateUnknown, g_activationCallCount == 1U);

    ResetAdapter(~Foundation::UInt64{0U});
    const Ia64RuntimeRegionActivationError unknownStatus = Ia64RuntimeRegionActivator::Activate(regionSet);
    ASTERIA_EXPECT(runner, "every unknown helper status maps conservatively to committed-state unknown",
                   unknownStatus == Ia64RuntimeRegionActivationError::CommittedStateUnknown,
                   g_activationCallCount == 1U);
}

void TestPhysicalBootstrapActivation(TestContext& runner) noexcept
{
    Ia64RuntimeRegionSet regionSet;
    const bool built = BuildRegionSet(regionSet);
    const Ia64BootstrapRegionRegisterWords expected{BootstrapWords()};

    ResetAdapter(AdapterSuccess);
    const Ia64RuntimeRegionActivationError success = Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet);
    bool exactForwarding{g_activationCallCount == 1U};
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        exactForwarding =
            exactForwarding && (g_forwardedRegionRegisters[index] == expected.RegionRegisters[index + 1U]);
    }
    ASTERIA_EXPECT(runner, "physical bootstrap forwards the same exact seven private RR words", built,
                   success == Ia64RuntimeRegionActivationError::None, exactForwarding);

    ResetAdapter(AdapterRejectedBeforeCommit);
    ASTERIA_EXPECT(runner, "physical bootstrap preserves a native precommit rejection",
                   Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet) ==
                       Ia64RuntimeRegionActivationError::RejectedBeforeCommit,
                   g_activationCallCount == 1U);

    ResetAdapter(AdapterCommittedStateUnknown);
    ASTERIA_EXPECT(runner, "physical bootstrap preserves committed-state uncertainty",
                   Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet) ==
                       Ia64RuntimeRegionActivationError::CommittedStateUnknown,
                   g_activationCallCount == 1U);

    ResetAdapter(~Foundation::UInt64{0U});
    ASTERIA_EXPECT(runner, "physical bootstrap maps an unknown native status conservatively",
                   Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet) ==
                       Ia64RuntimeRegionActivationError::CommittedStateUnknown,
                   g_activationCallCount == 1U);

    ResetAdapter(AdapterSuccess);
    const bool firstRepeat =
        Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet) == Ia64RuntimeRegionActivationError::None;
    const bool secondRepeat =
        Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap(regionSet) == Ia64RuntimeRegionActivationError::None;
    ASTERIA_EXPECT(runner, "the stateless physical activator revalidates each direct invocation", firstRepeat,
                   secondRepeat, g_activationCallCount == 2U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeRegionActivatorTests"};
    Asteria::Test::TestUnbuiltAndAnchorValidation(runner);
    Asteria::Test::TestPrivateWordValidation(runner);
    Asteria::Test::TestExactForwardingAndSuccess(runner);
    Asteria::Test::TestMinimumRepresentablePrivateRid(runner);
    Asteria::Test::TestMaximumRepresentablePrivateRid(runner);
    Asteria::Test::TestNativeStatusMapping(runner);
    Asteria::Test::TestPhysicalBootstrapActivation(runner);
    return runner.Finish();
}
