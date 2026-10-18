// SPDX-License-Identifier: Apache-2.0
#include "User/Session/ClientEvents.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace S = User::Session;
S::ClientEvents state;
S::SessionEvent answer[7U];
S::SessionEvent frozen[7U];
int main()
{
    TestSupport::TestContext t{"ClientEventsEdgeTests"};
    using K = S::SessionEventKind;
    ASTERIA_EXPECT(t, "key make and frozen answer", state.Queue({K::Key, {1U, 1U, 4U, 0U}}, 1U), state.Credit(7U, 1U),
                   state.Build(frozen, 1U) == 1U);
    state.SendFailed(true, 1U);
    for (Foundation::Size i{0U}; i < 63U; ++i)
    {
        static_cast<void>(state.Queue({K::Wheel, {1U, 1U, 0U, 0U}}, 2U));
    }
    ASTERIA_EXPECT(t, "overflow while reply blocked", state.Queue({K::PointerButtons, {1U, 0U, 1U, 0U}}, 3U),
                   state.Dropped() == 64U, state.AnswerDropped() == 0U, !state.AnswerMorePending(),
                   state.Queued() == 2U, state.NotResponding());
    ASTERIA_EXPECT(t, "frozen answer remains byte identical", state.Build(answer, 10001U) == 1U,
                   std::memcmp(answer[0U].Words, frozen[0U].Words, sizeof(answer[0U].Words)) == 0,
                   answer[0U].Kind == frozen[0U].Kind);
    state.Commit(10001U);
    ASTERIA_EXPECT(t, "post-freeze drops remain for next answer", state.Dropped() == 64U, state.Credit(7U, 10002U),
                   state.Build(answer, 10002U) == 1U, answer[0U].Kind == K::Key,
                   answer[0U].Words[1U] == S::SessionKeyUp);
    state.Commit(10002U);
    state.Reset();
    ASTERIA_EXPECT(t, "delivered button state", state.Queue({K::PointerButtons, {2U, 0U, 3U, 0U}}, 1U),
                   state.Credit(7U, 1U), state.Build(answer, 1U) == 1U);
    state.Commit(1U);
    for (Foundation::Size i{0U}; i < 64U; ++i)
    {
        static_cast<void>(state.Queue({K::Wheel, {1U, 1U, 0U, 0U}}, 2U));
    }
    ASTERIA_EXPECT(t, "button releases fit after overflow", state.Queue({K::Wheel, {1U, 1U, 0U, 0U}}, 2U),
                   state.Credit(7U, 3U), state.Build(answer, 3U) == 1U, answer[0U].Kind == K::PointerButtons,
                   answer[0U].Words[0U] == 2U, answer[0U].Words[2U] == 0U);
    state.Commit(3U);
    state.Reset();
    ASTERIA_EXPECT(t, "request refusal before in-flight change", state.Queue({K::RequestRefused, {1U, 1U, 2U, 0U}}, 1U),
                   state.Credit(7U, 1U), state.Build(answer, 1U) == 1U,
                   state.Queue({K::RequestRefused, {2U, 2U, 3U, 0U}}, 2U));
    state.Commit(3U);
    ASTERIA_EXPECT(t, "in-flight refusal counts aren't repeated", state.Credit(7U, 4U), state.Build(answer, 4U) == 1U,
                   answer[0U].Words[0U] == 2U, answer[0U].Words[2U] == 3U);
    state.SendFailed(false, 4U);
    ASTERIA_EXPECT(t, "non-blocking failure retains event for fresh credit", state.FailedSends() == 1U,
                   state.Build(answer, 5U) == 0U, state.Credit(7U, 5U), state.Build(answer, 5U) == 1U,
                   answer[0U].Words[2U] == 3U);
    state.Reset();
    ASTERIA_EXPECT(t, "credit offered by an unread Endpoint", state.Queue({K::Key, {1U, 1U, 4U, 0U}}, 1U),
                   state.Credit(7U, 1U), state.Build(answer, 1U) == 1U);
    state.SendFailed(true, 1U);
    state.Tick(S::ClientTimeoutUs + 1U);
    ASTERIA_EXPECT(t, "blocked credit cannot hide a stalled client", state.NotResponding());
    state.Reset();
    ASTERIA_EXPECT(t, "close starts one liveness deadline", state.Queue({K::CloseAsked, {1U, 0U, 0U, 0U}}, 1U));
    static_cast<void>(state.Queue({K::CloseAsked, {1U, 0U, 0U, 0U}}, S::ClientTimeoutUs));
    state.Tick(S::ClientTimeoutUs + 1U);
    ASTERIA_EXPECT(t, "repeated close cannot extend the first deadline", state.NotResponding());
    state.DestroySurface(1U);
    static_cast<void>(state.Credit(7U, S::ClientTimeoutUs + 2U));
    state.Tick(2U * S::ClientTimeoutUs);
    ASTERIA_EXPECT(t, "destroy removes the close liveness debt", !state.NotResponding());
    return t.Finish();
}
