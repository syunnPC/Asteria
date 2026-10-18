// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Bot.hpp"

#include <cstring>
#include <memory>

#include "Support/TestContext.hpp"
#include "User/Usb/FakeBot.hpp"

namespace
{
namespace M = Asteria::User::Usb;
using Asteria::TestSupport::TestContext;
constexpr M::BotBinding Binding{.Interface = 3U};
bool Drive(TestContext& context, M::Bot& bot, Asteria::TestSupport::FakeBot& fake, M::BotResult& result) noexcept
{
    for (unsigned i{0U}; i < 40U; ++i)
    {
        bot.Step(fake);
        if (fake.Held)
        {
            const auto reply = fake.Reply();
            ASTERIA_EXPECT(context, "BOT completion", bot.Complete(reply), !bot.Complete(reply), fake.Valid);
        }
        if (bot.TakeResult(result))
        {
            return true;
        }
    }
    return ASTERIA_EXPECT(context, "BOT bounded", false);
}
} // namespace
int main()
{
    TestContext context{"BOT transport"};
    M::UInt8 bytes[64U]{};
    M::ScsiCommand command = M::Scsi::InquiryCommand();
    ASTERIA_EXPECT(context, "CBW wire", M::BotCodec::Cbw(command, 0x12345678U, bytes, 64U), bytes[0U] == 0x55U,
                   bytes[1U] == 0x53U, bytes[2U] == 0x42U, bytes[3U] == 0x43U, bytes[4U] == 0x78U, bytes[7U] == 0x12U,
                   bytes[8U] == 36U, bytes[12U] == 0x80U, bytes[13U] == 0U, bytes[14U] == 6U, bytes[15U] == 0x12U);
    command.Length = 17U;
    M::UInt8 before[64U]{};
    std::memcpy(before, bytes, 64U);
    ASTERIA_EXPECT(context, "CBW invalid atomic", !M::BotCodec::Cbw(command, 0U, bytes, 64U),
                   std::memcmp(before, bytes, 64U) == 0);
    M::Csw csw{.Residue = 99U, .Status = 1U};
    constexpr M::UInt8 valid[]{0x55U, 0x53U, 0x42U, 0x53U, 1U, 0U, 0U, 0U, 4U, 0U, 0U, 0U, 0U};
    std::memcpy(bytes, valid, 13U);
    ASTERIA_EXPECT(context, "CSW residue", M::BotCodec::DecodeCsw(bytes, 13U, 1U, 8U, csw), csw.Residue == 4U,
                   !M::BotCodec::DecodeCsw(bytes, 12U, 1U, 8U, csw), !M::BotCodec::DecodeCsw(bytes, 14U, 1U, 8U, csw),
                   !M::BotCodec::DecodeCsw(bytes, 13U, 2U, 8U, csw), !M::BotCodec::DecodeCsw(bytes, 13U, 1U, 3U, csw));
    bytes[12U] = 2U;
    ASTERIA_EXPECT(context, "phase residue undefined", M::BotCodec::DecodeCsw(bytes, 13U, 1U, 0U, csw));
    bytes[12U] = 3U;
    ASTERIA_EXPECT(context, "invalid status", !M::BotCodec::DecodeCsw(bytes, 13U, 1U, 8U, csw));
    for (unsigned variant{0U}; variant < 5U; ++variant)
    {
        auto fake = std::make_unique<Asteria::TestSupport::FakeBot>();
        M::Bot bot{Binding};
        fake->FaultOpcode = 0x12U;
        fake->StallData = (variant == 0U) ? 1U : 0U;
        fake->ShortBy = (variant == 1U) ? 4U : 0U;
        fake->ExtraResidue = (variant == 2U) ? 4U : 0U;
        fake->ShortBy = (variant == 3U) ? 4U : fake->ShortBy;
        fake->ExtraResidue = (variant == 3U) ? ~M::UInt32{0U} - 3U : fake->ExtraResidue;
        fake->PhaseError = variant == 4U;
        M::BotResult result{};
        ASTERIA_EXPECT(context, "inline command", bot.Begin(M::Scsi::InquiryCommand()),
                       !bot.Begin(M::Scsi::ReadyCommand()));
        if (!Drive(context, bot, *fake, result))
        {
            continue;
        }
        if (variant >= 3U)
        {
            ASTERIA_EXPECT(context, "inconsistent relevant bytes reset", result.Status == M::BotStatus::TransportError,
                           result.StateLost, fake->Resets == 1U);
        }
        else
        {
            ASTERIA_EXPECT(context, "inline relevant data", result.Status == M::BotStatus::Passed,
                           result.RelevantLength == ((variant == 0U) ? 36U : 32U), result.Inline[8U] == 'A',
                           !result.StateLost);
        }
    }
    for (unsigned phase{0U}; phase < 4U; ++phase)
    {
        auto recovery = std::make_unique<Asteria::TestSupport::FakeBot>();
        M::Bot owner{Binding};
        M::BotResult outcome{};
        recovery->FaultOpcode = 0U;
        recovery->ErrorStage = 1U;
        ASTERIA_EXPECT(context, "recovery command", owner.Begin(M::Scsi::ReadyCommand()));
        owner.Step(*recovery);
        const auto failed = recovery->Reply();
        ASTERIA_EXPECT(context, "CBW recovery", owner.Complete(failed));
        if (phase == 3U)
        {
            ASTERIA_EXPECT(context, "cancel queued recovery", owner.Cancel(*recovery));
        }
        else
        {
            for (unsigned stage{0U}; stage <= phase; ++stage)
            {
                owner.Step(*recovery);
                auto completion = recovery->Reply();
                if (stage == phase)
                {
                    completion.Status = M::PipeStatus::Cancelled;
                }
                ASTERIA_EXPECT(context, "recovery completion", owner.Complete(completion));
            }
        }
        ASTERIA_EXPECT(context, "cancelled recovery terminates", owner.IsFaulted(), owner.TakeResult(outcome),
                       outcome.Status == M::BotStatus::Cancelled, outcome.StateLost,
                       !owner.Begin(M::Scsi::ReadyCommand()), recovery->Live == 0U);
    }
    auto fake = std::make_unique<Asteria::TestSupport::FakeBot>();
    M::Bot bot{Binding};
    ASTERIA_EXPECT(context, "pending readiness", bot.Begin(M::Scsi::ReadyCommand()));
    bot.Step(*fake);
    M::BotResult result{};
    fake->AbortRefused = true;
    ASTERIA_EXPECT(context, "refused abort keeps borrow", !bot.Cancel(*fake), bot.IsOutstanding(),
                   !bot.TakeResult(result));
    M::PipeCompletion stale{.Token = fake->Pending.Token + 1U};
    ASTERIA_EXPECT(context, "stale token rejected", !bot.Complete(stale), bot.IsOutstanding());
    fake->AbortRefused = false;
    ASTERIA_EXPECT(context, "abort", bot.Cancel(*fake), bot.IsOutstanding());
    const auto reply = fake->Reply();
    ASTERIA_EXPECT(context, "cancel completion", bot.Complete(reply));
    Drive(context, bot, *fake, result);
    ASTERIA_EXPECT(context, "cancel recovery", result.Status == M::BotStatus::Cancelled, result.StateLost);
    ASTERIA_EXPECT(context, "cancel before CBW", bot.Begin(M::Scsi::ReadyCommand()), bot.Cancel(*fake),
                   bot.TakeResult(result), result.Status == M::BotStatus::Cancelled, !result.StateLost);
    return context.Finish();
}
