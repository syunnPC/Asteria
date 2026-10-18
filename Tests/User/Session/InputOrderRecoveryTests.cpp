// SPDX-License-Identifier: Apache-2.0
#include "User/Session/InputOrder.hpp"
#include "Support/TestContext.hpp"
int main()
{
    namespace TestSupport = Asteria::TestSupport;
    namespace User = Asteria::User;
    TestSupport::TestContext test{"InputOrderRecoveryTests"};
    User::Session::InputOrder order{};
    order.Configure(9U, 256U);
    order.Resume(8U, 10U);
    ASTERIA_EXPECT(test, "queued recovery key has an already-fenced pointer horizon", order.Keys(10U, 2U, 1U, 0U),
                   order.Pointer(11U, 10U, 2U, 0U));
    ASTERIA_EXPECT(test, "keys and pointer resume without a dependency cycle",
                   order.Take(1U).Type == User::Session::InputOrder::Kind::Keys,
                   order.Take(1U).Type == User::Session::InputOrder::Kind::Pointer, order.AppliedKeys() == 10U,
                   order.AppliedPointer() == 11U);
    ASTERIA_EXPECT(test, "a late snapshot before the baseline is refused", !order.Pointer(10U, 8U, 3U, 0U));
    order.Configure(9U, 257U);
    order.Resume(10U, 11U);
    ASTERIA_EXPECT(test, "second replacement advances both bases", order.Pointer(12U, 10U, 1U, 0U),
                   order.Take(1U).Type == User::Session::InputOrder::Kind::Pointer);
    return test.Finish();
}
