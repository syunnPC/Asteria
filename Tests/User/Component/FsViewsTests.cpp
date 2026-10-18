// SPDX-License-Identifier: Apache-2.0
#include "User/Component/FsViews.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
void TestViews(TestSupport::TestContext& test) noexcept
{
    static User::Component::FsViews views;
    views.Initialize();
    const User::Component::FsNode root{.Object = 100U, .Birth = 4U, .Kind = User::Filesystem::NodeKind::Directory};
    auto* first = views.Install(11U, 7U, root);
    auto* other = views.Install(12U, 1U, root);
    ASTERIA_EXPECT(test, "views are identity bound and installation validates directory/rights", first != nullptr,
                   other != nullptr, views.Find(first->Id, 12U) == nullptr, views.Install(0U, 7U, root) == nullptr,
                   views.Install(11U, 8U, root) == nullptr, views.Install(11U, 1U, {}) == nullptr);
    const auto a = views.Open(*first, {.Object = 101U, .Birth = 4U, .Kind = User::Filesystem::NodeKind::Directory}, 0U);
    const auto b = views.Open(*first, {.Object = 102U, .Birth = 4U}, a);
    const auto old = first->Slots[a].Generation;
    views.Close(*first, a);
    ASTERIA_EXPECT(test, "closed parents stay pinned for lexical parent traversal and reopen with a fresh generation",
                   views.Resolve(*first, a, old) == nullptr, views.Parent(*first, b) == a,
                   first->Slots[a].Generation != old, views.Resolve(*first, a, old) == nullptr,
                   views.Resolve(*other, a, first->Slots[a].Generation) == nullptr, views.Parent(*first, 0U) == 0U);
    const auto oldRoot = first->Slots[0U].Generation;
    views.Close(*first, 0U);
    ASTERIA_EXPECT(test, "closing the reserved root rejects its old generation; a child can reopen its lexical parent",
                   views.Resolve(*first, 0U, oldRoot) == nullptr, views.Parent(*first, a) == 0U,
                   first->Slots[0U].Generation != oldRoot);
    views.Close(*first, a);
    views.Close(*first, b);
    ASTERIA_EXPECT(test, "closing the child releases closed ancestors and slot reuse never revives stale generations",
                   !first->Slots[a].Used, !first->Slots[b].Used,
                   views.Open(*first, {.Object = 103U, .Birth = 4U}, 0U) == a,
                   views.Resolve(*first, a, old) == nullptr);
    bool filled{true};
    for (Foundation::Size n{2U}; n < User::Component::FsOpenSlots; ++n)
    {
        filled = filled && views.Open(*first, {.Object = 200U + n, .Birth = 4U}, 0U) == n;
    }
    ASTERIA_EXPECT(test, "the 64-slot table refuses exhaustion without replacing an open entry", filled,
                   !views.HasFree(*first), views.Open(*first, {}, 0U) == User::Component::FsOpenSlots);
    const auto occupied = first->Slots[a].Generation;
    views.InvalidateOwnerSlots(11U);
    ASTERIA_EXPECT(test, "aborting a writer invalidates its pending slots and preserves another identity's authority",
                   views.Resolve(*first, a, occupied) == nullptr, views.HasFree(*first),
                   first->Slots[0U].Children == 0U, views.Resolve(*other, 0U, other->Id) != nullptr);
    const auto id = first->Id;
    views.Reset();
    const auto* reinstalled = views.Install(11U, 7U, root);
    ASTERIA_EXPECT(test, "reset invalidates every view and identifiers stay monotonic", views.Find(id, 11U) == nullptr,
                   reinstalled != nullptr, reinstalled->Id != id);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext test{"FsViewsTests"};
    TestViews(test);
    return test.Finish();
}
