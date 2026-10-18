// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialTranslationActivator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
constinit Foundation::Size g_activationCallCount{0U};
constinit Foundation::UInt64 g_activationStatus{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateInitialTranslation() noexcept
{
    ++Asteria::Test::g_activationCallCount;
    return Asteria::Test::g_activationStatus;
}

namespace Asteria::Test
{
namespace
{
using Platform::Ia64::Memory::InitialTranslationActivationError;
using Platform::Ia64::Memory::InitialTranslationActivator;

static_assert(sizeof(InitialTranslationActivationError) == sizeof(Foundation::UInt32));

void CheckStatus(TestContext& runner, const Foundation::UInt64 status,
                 const InitialTranslationActivationError expectedError, const char* const description) noexcept
{
    g_activationCallCount = 0U;
    g_activationStatus = status;
    const InitialTranslationActivationError error = InitialTranslationActivator::Activate();
    ASTERIA_EXPECT(runner, description, error == expectedError, g_activationCallCount == 1U);
}

void TestAdapterStatusMapping(TestContext& runner) noexcept
{
    CheckStatus(runner, 1U, InitialTranslationActivationError::UnsupportedProcessorState,
                "status one reports unsupported processor state after exactly one call");
    CheckStatus(runner, 0U, InitialTranslationActivationError::UnexpectedAdapterReturn,
                "an impossible success return fails closed after exactly one call");
    CheckStatus(runner, 2U, InitialTranslationActivationError::UnexpectedAdapterReturn,
                "an unknown nonzero return fails closed after exactly one call");
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialTranslationActivatorTests"};
    Asteria::Test::TestAdapterStatusMapping(runner);
    return runner.Finish();
}
