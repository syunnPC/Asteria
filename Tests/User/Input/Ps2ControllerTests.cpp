// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2Controller.hpp"

#include <cstdio>
#include <vector>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

#include "User/Input/Ps2ControllerModel.hpp"

namespace In = Asteria::User::Input;
using Asteria::Test::Input::Model;
using Asteria::Test::Input::Byte;

namespace
{
// Reference for the keyboard path: the mode, and the step and reason at
// which the scan-set-2 path fails (None when it succeeds).
struct Expected
{
    In::Ps2KeyboardMode Mode;
    In::Ps2Step Step;
    In::Ps2Reason Reason;
};
[[nodiscard]] Expected Reference(const Model& model, const bool firmwareTranslates) noexcept
{
    if (model.MuteConfiguration)
    {
        return {In::Ps2KeyboardMode::Set1Translated, In::Ps2Step::ReadConfiguration, In::Ps2Reason::Timeout};
    }
    if (model.TranslationLocked)
    {
        return {In::Ps2KeyboardMode::Set1Translated, In::Ps2Step::VerifyConfiguration, In::Ps2Reason::Unexpected};
    }
    const In::Ps2KeyboardMode firmware =
        firmwareTranslates ? In::Ps2KeyboardMode::Set1Translated : In::Ps2KeyboardMode::Set2Unnegotiated;
    if (model.SilentKeyboard)
    {
        return {firmware, In::Ps2Step::KeyboardDisable, In::Ps2Reason::Timeout};
    }
    if (model.RejectScanSet)
    {
        return {firmware, In::Ps2Step::KeyboardGetSet, In::Ps2Reason::Resend};
    }
    if (model.TranslatedAnswer)
    {
        return {In::Ps2KeyboardMode::Set1Translated, In::Ps2Step::KeyboardGetSet, In::Ps2Reason::Unexpected};
    }
    return {In::Ps2KeyboardMode::Set2, In::Ps2Step::None, In::Ps2Reason::Unexpected};
}
[[nodiscard]] unsigned Count(const Model& model, const unsigned write) noexcept
{
    unsigned count{0U};
    for (const unsigned value : model.Writes)
    {
        count += (value == write) ? 1U : 0U;
    }
    return count;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"Ps2Controller"};
    Model exact{};
    In::Ps2Controller controller{exact.Transport(), 256U};
    constexpr unsigned expected[]{0x1ADU, 0x1A7U, 0x120U, 0x160U, 0x35U,  0x120U, 0x1AEU, 0xF5U,  0xF0U,
                                  0U,     0xF4U,  0x160U, 0x27U,  0x1A8U, 0x1D4U, 0xF6U,  0x1D4U, 0xEAU,
                                  0x1D4U, 0xF3U,  0x1D4U, 200U,   0x1D4U, 0xF3U,  0x1D4U, 100U,   0x1D4U,
                                  0xF3U,  0x1D4U, 80U,    0x1D4U, 0xF2U,  0x1D4U, 0xF4U};
    ASTERIA_EXPECT(context, "complete controller programming protocol",
                   controller.Initialize(In::AuxiliaryPolicy::Probe), exact.Valid,
                   exact.Writes == std::vector<unsigned>(std::begin(expected), std::end(expected)),
                   exact.Output.empty(), controller.GetMouseMode() == In::MouseMode::IntelliMouse,
                   controller.GetKeyboardMode() == In::Ps2KeyboardMode::Set2, (exact.Config & 0x40U) == 0U);
    ASTERIA_EXPECT(context, "quiet proof disables both ports", controller.Quiesce(), (exact.Config & 48U) == 48U);
    const auto seeds = Asteria::TestSupport::GetSeedRange(8042U, 8U);
    for (In::Size seedAt{0U}; seedAt < seeds.Count; ++seedAt)
    {
        Asteria::TestSupport::Random random{seeds.First + seedAt};
        for (In::Size step{0U}; step < 64U; ++step)
        {
            const auto failuresBefore = context.GetFailureCount();
            Model model{};
            const bool probe = random.Below(2U) == 0U;
            const bool auxFails = random.Below(3U) == 0U;
            model.AuxAbsent = auxFails;
            model.Wheel = random.Below(2U) == 0U;
            model.Latency = static_cast<unsigned>(random.Below(100U));
            model.Resends = static_cast<unsigned>(random.Below(3U));
            model.AuxResends = static_cast<unsigned>(random.Below(3U));
            model.ScanSet = random.Below(2U) == 0U ? 1U : 2U;
            model.Inject = true;
            model.InjectAux = probe && !auxFails;
            model.Output.push_back(Byte{0U, 0x1CU});
            In::Ps2Controller programmed{model.Transport(), 256U};
            In::AuxiliaryState expectedAuxiliary{In::AuxiliaryState::Disabled};
            if (probe)
            {
                expectedAuxiliary = auxFails ? In::AuxiliaryState::Failed : In::AuxiliaryState::Ready;
            }
            ASTERIA_EXPECT(context, "seeded ACK/RESEND, slow keyboard, standard mouse, interleaved bytes",
                           programmed.Initialize(probe ? In::AuxiliaryPolicy::Probe : In::AuxiliaryPolicy::Disabled),
                           model.Valid, model.ScanSet == 2U, model.Observed.size() == (probe && !auxFails ? 5U : 3U),
                           model.Output.empty(), programmed.GetAuxiliaryState() == expectedAuxiliary,
                           programmed.GetMouseMode() == (probe && !auxFails && model.Wheel ? In::MouseMode::IntelliMouse
                                                                                           : In::MouseMode::Standard));
            if (context.GetFailureCount() != failuresBefore)
            {
                std::fprintf(stderr, "seed=%llu step=%zu\n", static_cast<unsigned long long>(seeds.First + seedAt),
                             step);
                return context.Finish();
            }
        }
    }
    for (unsigned kind{0U}; kind < 2U; ++kind)
    {
        Model broken{};
        broken.Stuck = kind == 0U;
        broken.Absent = kind == 1U;
        In::Ps2Controller bounded{broken.Transport(), 256U};
        const bool failed = !bounded.Initialize(In::AuxiliaryPolicy::Probe);
        const In::Ps2Diagnostic failure = bounded.GetFailure();
        ASTERIA_EXPECT(context, "absence and stuck input fail at the first command with their evidence", failed,
                       failure.Step == In::Ps2Step::DisableKeyboard, failure.Status == (kind == 0U ? 2U : 0xFFU),
                       failure.Reason == (kind == 0U ? In::Ps2Reason::Timeout : In::Ps2Reason::Unexpected),
                       !failure.DataValid, broken.Writes.empty());
    }
    for (unsigned kind{0U}; kind < 2U; ++kind)
    {
        // An ACK timeout and RESEND exhaustion of the keyboard fall back to
        // the firmware's translation (0x47 has it on) instead of failing.
        Model slow{};
        slow.Latency = kind == 0U ? 300U : 0U;
        slow.Resends = kind == 1U ? 3U : 0U;
        In::Ps2Controller bounded{slow.Transport(), 256U};
        ASTERIA_EXPECT(
            context, "keyboard ACK timeout and RESEND exhaustion fall back to set 1",
            bounded.Initialize(In::AuxiliaryPolicy::Disabled),
            bounded.GetKeyboardMode() == In::Ps2KeyboardMode::Set1Translated,
            bounded.GetFallbackReason().Step == In::Ps2Step::KeyboardDisable,
            bounded.GetFallbackReason().Reason == (kind == 0U ? In::Ps2Reason::Timeout : In::Ps2Reason::Resend),
            bounded.GetFallbackReason().Command == 0xF5U, (slow.Config & 0x40U) != 0U, Count(slow, 0xF0U) == 0U);
    }
    {
        // A controller slower than the poll floor is not dead when a clock
        // shows the wait has not reached WaitMilliseconds yet.
        Model slow{};
        slow.Latency = 400U;
        slow.TicksPerRead = 1U;
        In::Ps2Controller clocked{slow.Transport(), 256U, 1U};
        ASTERIA_EXPECT(context, "clocked waits outlast a slow keyboard",
                       clocked.Initialize(In::AuxiliaryPolicy::Disabled),
                       clocked.GetKeyboardMode() == In::Ps2KeyboardMode::Set2, slow.Valid);
        Model stopped{};
        stopped.Stuck = true;
        In::Ps2Controller frozen{stopped.Transport(), 4U, 1U};
        ASTERIA_EXPECT(context, "a stopped clock still ends the wait",
                       !frozen.Initialize(In::AuxiliaryPolicy::Disabled),
                       frozen.GetFailure().Reason == In::Ps2Reason::Timeout, stopped.Ticks == 0U);
    }
    {
        const In::Ps2Diagnostic sample{.Step = In::Ps2Step::KeyboardGetSet,
                                       .Reason = In::Ps2Reason::Resend,
                                       .DataValid = true,
                                       .Command = 0xF0U,
                                       .Status = 0x1CU,
                                       .Data = 0xFEU};
        const In::Ps2Diagnostic decoded = In::DecodePs2Diagnostic(In::EncodePs2Diagnostic(sample));
        ASTERIA_EXPECT(context, "diagnostic wire form round trip", In::EncodePs2Diagnostic(sample) == 0xFE1CF0CAU,
                       decoded.Step == sample.Step, decoded.Reason == sample.Reason, decoded.DataValid,
                       decoded.Command == 0xF0U, decoded.Status == 0x1CU, decoded.Data == 0xFEU,
                       In::DecodePs2Diagnostic(31U).Step == In::Ps2Step::None);
    }
    // Seeded embedded-controller behaviours against the reference: the mode,
    // the recorded set-2 failure, the final translation bit, and that no
    // scan-set command follows a refused translation change.
    const auto controllerSeeds = Asteria::TestSupport::GetSeedRange(8043U, 8U);
    for (In::Size seedAt{0U}; seedAt < controllerSeeds.Count; ++seedAt)
    {
        Asteria::TestSupport::Random random{controllerSeeds.First + seedAt};
        for (In::Size step{0U}; step < 64U; ++step)
        {
            const auto failuresBefore = context.GetFailureCount();
            Model model{};
            const unsigned behaviour = static_cast<unsigned>(random.Below(6U));
            model.MuteConfiguration = behaviour == 1U;
            model.TranslationLocked = behaviour == 2U;
            model.SilentKeyboard = behaviour == 3U;
            model.RejectScanSet = behaviour == 4U;
            model.TranslatedAnswer = behaviour == 5U;
            model.Unsolicited = random.Below(2U) == 0U;
            const bool firmwareTranslates = random.Below(2U) == 0U;
            model.Config = firmwareTranslates ? 0x47U : 0x07U;
            model.ScanSet = random.Below(2U) == 0U ? 1U : 2U;
            const bool clocked = random.Below(2U) == 0U;
            model.TicksPerRead = clocked ? 1U : 0U;
            model.Latency = static_cast<unsigned>(random.Below(clocked ? 450U : 200U));
            const bool probe = random.Below(2U) == 0U;
            In::Ps2Controller programmed{model.Transport(), 256U, clocked ? 1U : 0U};
            const Expected reference = Reference(model, firmwareTranslates);
            const bool initialized =
                programmed.Initialize(probe ? In::AuxiliaryPolicy::Probe : In::AuxiliaryPolicy::Disabled);
            const In::Ps2Diagnostic& reason = programmed.GetFallbackReason();
            const bool translated = reference.Mode == In::Ps2KeyboardMode::Set1Translated;
            ASTERIA_EXPECT(context, "embedded-controller fallback follows the reference", initialized, model.Valid,
                           programmed.GetKeyboardMode() == reference.Mode, reason.Step == reference.Step,
                           (reference.Step == In::Ps2Step::None) || (reason.Reason == reference.Reason),
                           model.MuteConfiguration || (((model.Config & 0x40U) != 0U) == translated),
                           !(model.MuteConfiguration || model.TranslationLocked) || (Count(model, 0xF0U) == 0U),
                           !model.TranslatedAnswer || ((reason.Data == 0x41U) || (reason.Data == 0x43U)),
                           (reference.Step != In::Ps2Step::None) || (model.ScanSet == 2U),
                           programmed.GetAuxiliaryState() ==
                               (probe
                                    ? (model.MuteConfiguration ? In::AuxiliaryState::Failed : In::AuxiliaryState::Ready)
                                    : In::AuxiliaryState::Disabled));
            if (context.GetFailureCount() != failuresBefore)
            {
                std::fprintf(stderr, "seed=%llu step=%zu behaviour=%u\n",
                             static_cast<unsigned long long>(controllerSeeds.First + seedAt), step, behaviour);
                return context.Finish();
            }
        }
    }
    Model keyboardOnly{};
    In::Ps2Controller keyboard{keyboardOnly.Transport(), 256U};
    ASTERIA_EXPECT(context, "keyboard only never enables or addresses aux",
                   keyboard.Initialize(In::AuxiliaryPolicy::Disabled),
                   keyboard.GetAuxiliaryState() == In::AuxiliaryState::Disabled);
    for (const unsigned write : keyboardOnly.Writes)
    {
        ASTERIA_EXPECT(context, "no aux enable or command", write != 0x1A8U, write != 0x1D4U);
    }
    Model failingAux{};
    failingAux.AuxResends = 3U;
    In::Ps2Controller fallback{failingAux.Transport(), 256U};
    ASTERIA_EXPECT(context, "aux failure keeps initialized keyboard", fallback.Initialize(In::AuxiliaryPolicy::Probe),
                   fallback.GetAuxiliaryState() == In::AuxiliaryState::Failed, (failingAux.Config & 16U) == 0U,
                   (failingAux.Config & 32U) != 0U);
    Model bytes{};
    bytes.Output.push_back(Byte{0x80U, 0x1CU});
    bytes.Output.push_back(Byte{32U, 8U});
    In::Ps2Controller drain{bytes.Transport(), 4U};
    ASTERIA_EXPECT(context, "parity bytes dropped, aux status retained", drain.Drain(), bytes.Observed.size() == 1U,
                   bytes.Observed[0U] == 0x2108U);
    return context.Finish();
}
