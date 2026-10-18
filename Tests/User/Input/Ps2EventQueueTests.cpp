// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2EventQueue.hpp"

#include <cstdio>
#include <vector>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

int main()
{
    namespace In = Asteria::User::Input;
    namespace C = Asteria::User::Component;
    Asteria::TestSupport::TestContext context{"Ps2EventQueue"};
    const auto seeds = Asteria::TestSupport::GetSeedRange(18042U, 8U);
    for (In::Size seedAt{0U}; seedAt < seeds.Count; ++seedAt)
    {
        Asteria::TestSupport::Random random{seeds.First + seedAt};
        In::Ps2EventQueue queue{};
        std::vector<int> model{};
        In::Size pending{0U};
        Asteria::Foundation::UInt64 transaction{1U};
        for (In::Size step{0U}; step < 4000U; ++step)
        {
            const auto failuresBefore = context.GetFailureCount();
            const auto operation = random.Below(7U);
            if (operation <= 1U)
            {
                const int delta = static_cast<int>(random.Below(200U)) - 100;
                const In::InputEvent event{.Kind = In::EventKind::Motion,
                                           .Modifiers = 0U,
                                           .Usage = 0U,
                                           .Source = 0U,
                                           .Dx = delta,
                                           .Dy = 0,
                                           .Buttons = 0U,
                                           .Reserved = {},
                                           .Wheel = 0};
                const bool space = model.size() < 64U;
                ASTERIA_EXPECT(context, "bounded FIFO append", queue.Append(event) == space);
                if (space)
                {
                    model.push_back(delta);
                }
            }
            else if (operation == 2U)
            {
                if (pending == 0U)
                {
                    pending = model.size() < 8U ? model.size() : 8U;
                }
                ASTERIA_EXPECT(context, "freeze up to eight events", queue.Prepare() == pending);
            }
            else if ((operation == 3U) || (operation == 4U))
            {
                ASTERIA_EXPECT(context, "retry retains batch and transaction",
                               queue.Answer(operation == 3U ? C::InputAnswer::Busy : C::InputAnswer::Exhausted));
            }
            else if (operation == 5U)
            {
                ASTERIA_EXPECT(context, "only an outstanding batch can commit",
                               queue.Answer(C::InputAnswer::Done) == (pending != 0U));
                if (pending != 0U)
                {
                    for (In::Size index{0U}; index < pending; ++index)
                    {
                        model.erase(model.begin());
                    }
                    pending = 0U;
                    ++transaction;
                }
            }
            else
            {
                ASTERIA_EXPECT(context, "restart discards old input", queue.Answer(C::InputAnswer::Restarted));
                model.clear();
                pending = 0U;
            }
            ASTERIA_EXPECT(context, "model count and monotonic transaction", queue.GetCount() == model.size(),
                           queue.GetTransaction() == transaction);
            for (In::Size index{0U}; index < model.size(); ++index)
            {
                ASTERIA_EXPECT(context, "FIFO byte preservation across retries",
                               queue.GetEvents()[index].Dx == model[index]);
            }
            if (context.GetFailureCount() != failuresBefore)
            {
                std::fprintf(stderr, "seed=%llu step=%zu\n", static_cast<unsigned long long>(seeds.First + seedAt),
                             step);
                return context.Finish();
            }
        }
    }
    return context.Finish();
}
