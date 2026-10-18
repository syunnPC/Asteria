// SPDX-License-Identifier: Apache-2.0
#include "Support/TestContext.hpp"

namespace Asteria::User::Runtime
{
[[nodiscard]] bool ValidateStatusRegistry() noexcept;
}

int main()
{
    Asteria::TestSupport::TestContext context{"StatusRegistryTests"};
    ASTERIA_EXPECT(context, "the separately linked registry has unique facilities in their allocation ranges",
                   Asteria::User::Runtime::ValidateStatusRegistry());
    return context.Finish();
}
