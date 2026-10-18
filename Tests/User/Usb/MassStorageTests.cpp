// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/MassStorage.hpp"

#include <cstdio>
#include <cstring>
#include <memory>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Usb/FakeBot.hpp"

namespace
{
namespace M = Asteria::User::Usb;
using Asteria::TestSupport::FakeBot;
using Asteria::TestSupport::TestContext;
constexpr M::BotBinding Binding{.ControlPipe = 0U, .InPipe = 1U, .OutPipe = 2U, .Interface = 3U};
constexpr M::FrameBuffer Buffer{.Handle = {.Selector = 7U, .Authenticator = 9U}, .Offset = 512U, .Length = 4096U};

bool Drive(TestContext& context, M::MassStorage& storage, FakeBot& fake, M::StorageResult& result,
           M::UInt64& now) noexcept
{
    for (unsigned step{0U}; step < 500U; ++step)
    {
        storage.Step(fake, now);
        if (fake.Held)
        {
            const M::PipeCompletion reply = fake.Reply();
            ASTERIA_EXPECT(context, "completion ownership", storage.Complete(reply), !storage.Complete(reply),
                           fake.Live == 0U, fake.MaxLive == 1U, fake.Valid);
        }
        if (storage.TakeResult(result))
        {
            ASTERIA_EXPECT(context, "exactly one final result", !storage.TakeResult(result));
            return true;
        }
        now += 10U;
    }
    ASTERIA_EXPECT(context, "bounded operation", false);
    return false;
}
bool Describe(TestContext& context, M::MassStorage& storage, FakeBot& fake, M::StorageResult& result,
              M::UInt64& now) noexcept
{
    return ASTERIA_EXPECT(context, "Describe accepted", storage.Describe()) &&
           Drive(context, storage, fake, result, now);
}
void Discovery(TestContext& context)
{
    for (unsigned variant{0U}; variant < 13U; ++variant)
    {
        auto fake = std::make_unique<FakeBot>();
        M::MassStorage storage{Binding};
        M::StorageResult result{};
        M::UInt64 now{0U};
        fake->LunStall = variant == 1U;
        fake->MaxLun = (variant == 2U) ? 7U : 0U;
        fake->NotReady = (variant == 3U) ? 3U : ((variant == 4U) ? 4U : 0U);
        fake->Attention = (variant == 5U) ? 0x28U : ((variant == 6U) ? 0x29U : 0U);
        fake->DescriptorSense = variant == 6U;
        fake->ReadOnly = variant == 7U;
        fake->Mode6Refused = variant == 7U;
        fake->BlockCount = (variant == 8U) ? 0x100000001ULL : 256U;
        fake->BlockSize = (variant == 9U) ? 4096U : 512U;
        fake->NoMedium = variant == 10U;
        fake->MaxLun = (variant == 11U) ? 16U : fake->MaxLun;
        if (variant == 12U)
        {
            fake->FailOpcode = 0U;
            fake->FailKey = 6U;
            fake->FailAsc = 0x28U;
        }
        if (!Describe(context, storage, *fake, result, now))
        {
            continue;
        }
        if (variant == 12U)
        {
            ASTERIA_EXPECT(context, "UNIT ATTENTION retry budget", result.Status == M::StorageStatus::MediumChanged,
                           fake->Commands[0U] == 4U, fake->Commands[3U] == 4U, storage.Generation() == 5U,
                           !storage.HasDescription());
        }
        else if (variant == 4U)
        {
            ASTERIA_EXPECT(context, "readiness bounded", result.Status == M::StorageStatus::NotReady,
                           fake->Commands[0U] == 4U, fake->Commands[3U] == 4U, !storage.HasDescription(), now >= 300U);
        }
        else if (variant == 9U)
        {
            ASTERIA_EXPECT(context, "4Kn unsupported", result.Status == M::StorageStatus::Unsupported,
                           !storage.HasDescription(), result.Description.BlockCount == 0U);
        }
        else if (variant == 11U)
        {
            ASTERIA_EXPECT(context, "invalid maximum LUN", result.Status == M::StorageStatus::TransportError);
        }
        else
        {
            ASTERIA_EXPECT(context, "Describe snapshot", result.Status == M::StorageStatus::Ok,
                           storage.HasDescription(), result.Description.Removable, result.Description.BlockSize == 512U,
                           std::strcmp(result.Description.Vendor, "ASTERIA ") == 0,
                           std::strcmp(result.Description.Product, "TEST USB DISK   ") == 0,
                           result.Description.ReadOnly == fake->ReadOnly,
                           result.Description.MediumPresent == !fake->NoMedium,
                           result.Description.BlockCount == (fake->NoMedium ? 0U : fake->BlockCount));
            ASTERIA_EXPECT(context, "capacity fallback only on saturation",
                           fake->Commands[0x9EU] == ((variant == 8U) ? 1U : 0U));
            ASTERIA_EXPECT(context, "mode fallback on illegal request",
                           fake->Commands[0x5AU] == ((variant == 7U) ? 1U : 0U));
            ASTERIA_EXPECT(context, "attention generation",
                           storage.Generation() == (((variant == 5U) || (variant == 6U)) ? 2U : 1U));
        }
    }
}
void Errors(TestContext& context)
{
    for (unsigned variant{0U}; variant < 18U; ++variant)
    {
        auto fake = std::make_unique<FakeBot>();
        M::MassStorage storage{Binding};
        M::StorageResult result{};
        M::UInt64 now{0U};
        if (!Describe(context, storage, *fake, result, now))
        {
            continue;
        }
        const bool write = variant == 17U;
        fake->FaultOpcode = write ? 0x2AU : 0x28U;
        fake->StallCbw = (variant == 0U) ? 1U : 0U;
        fake->StallData = ((variant == 1U) || (variant == 17U)) ? 1U : 0U;
        fake->StallCsw = (variant == 2U) ? 1U : ((variant == 3U) ? 2U : 0U);
        fake->PhaseError = variant == 4U;
        fake->BadSignature = variant == 5U;
        fake->BadTag = variant == 6U;
        fake->ShortCsw = variant == 7U;
        fake->ShortBy = (variant == 8U) ? 512U : 0U;
        fake->ExtraResidue = (variant == 9U) ? 512U : ((variant == 10U) ? 4096U : 0U);
        fake->ErrorStage = (variant >= 11U && variant <= 13U) ? static_cast<M::UInt8>(variant - 10U) : 0U;
        if (variant == 14U)
        {
            fake->BadTag = true;
            fake->ResetFails = true;
        }
        if (variant == 15U)
        {
            fake->StallCsw = 1U;
            fake->ClearFails = true;
        }
        if (variant == 16U)
        {
            fake->FailOpcode = 0x28U;
        }
        ASTERIA_EXPECT(context, "range accepted",
                       write ? storage.Write(storage.Generation(), 4U, 4U, Buffer, 2048U)
                             : storage.Read(storage.Generation(), 4U, 4U, Buffer, 2048U));
        if (!Drive(context, storage, *fake, result, now))
        {
            continue;
        }
        const bool success = (variant == 1U) || (variant == 2U) || (variant == 17U);
        const bool ioError = (variant == 8U) || (variant == 9U) || (variant == 16U);
        ASTERIA_EXPECT(context, "BOT observable result",
                       result.Status ==
                           (success ? M::StorageStatus::Ok
                                    : (ioError ? M::StorageStatus::IoError : M::StorageStatus::TransportError)),
                       result.Blocks == (success ? 4U : 0U), fake->Resets == ((!success && !ioError) ? 1U : 0U));
        if (!success && !ioError)
        {
            ASTERIA_EXPECT(context, "reset loses cached state", storage.Generation() == 2U, !storage.HasDescription());
        }
        if (variant == 2U)
        {
            ASTERIA_EXPECT(context, "CSW clear then retry", fake->Clears[1U] == 1U);
        }
        if (variant == 17U)
        {
            ASTERIA_EXPECT(context, "OUT data stall clears OUT", fake->Clears[2U] == 1U);
        }
        if (variant == 14U || variant == 15U)
        {
            ASTERIA_EXPECT(context, "failed recovery terminal", storage.IsFaulted(), !storage.Describe());
        }
    }
}
void CancelAndBounds(TestContext& context)
{
    auto fake = std::make_unique<FakeBot>();
    M::MassStorage storage{Binding};
    M::StorageResult result{};
    M::UInt64 now{0U};
    if (!Describe(context, storage, *fake, result, now))
    {
        return;
    }
    ASTERIA_EXPECT(context, "range refusals atomic", !storage.Read(0U, 0U, 1U, Buffer, 512U),
                   !storage.Read(1U, 0U, 0U, Buffer, 512U), !storage.Read(1U, 255U, 2U, Buffer, 1024U),
                   !storage.Read(1U, ~M::UInt64{0U}, 1U, Buffer, 512U), !storage.Read(1U, 0U, 4U, Buffer, 512U),
                   !storage.Read(1U, 0U, 129U, Buffer, 65536U),
                   !storage.Read(1U, 0U, 1U, {.Offset = ~M::UInt64{0U}, .Length = 512U}, 512U),
                   storage.Generation() == 1U, storage.HasDescription());
    ASTERIA_EXPECT(context, "read accepted", storage.Read(1U, 0U, 1U, Buffer, 512U));
    storage.Step(*fake, now);
    ASTERIA_EXPECT(context, "abort accepted", fake->Held, storage.Cancel(*fake), fake->Live == 1U,
                   !storage.TakeResult(result));
    const M::PipeCompletion c = fake->Reply();
    ASTERIA_EXPECT(context, "abort keeps original completion", storage.Complete(c), !storage.Complete(c));
    Drive(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "cancelled after reset", result.Status == M::StorageStatus::Cancelled, fake->Aborts == 1U,
                   fake->Resets == 1U, storage.Generation() == 2U);
    fake->Refuse = 3U;
    ASTERIA_EXPECT(context, "reprobe", storage.Describe());
    Drive(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "submission refusal bounded", result.Status == M::StorageStatus::TransportError,
                   fake->Live == 0U);
}
void FacadeSemantics(TestContext& context)
{
    auto fake = std::make_unique<FakeBot>();
    M::MassStorage storage{Binding};
    M::StorageResult result{};
    M::UInt64 now{0U};
    if (!Describe(context, storage, *fake, result, now))
    {
        return;
    }
    const M::FrameBuffer full{.Handle = {.Selector = 7U, .Authenticator = 9U}, .Length = 65536U};
    std::memset(fake->Frame, 0x5A, sizeof(fake->Frame));
    ASTERIA_EXPECT(context, "maximum WRITE10", storage.Write(1U, 128U, 128U, full, 65536U));
    Drive(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "maximum write complete", result.Status == M::StorageStatus::Ok, result.Blocks == 128U,
                   fake->Disk[65536U] == 0x5AU, fake->Disk[131071U] == 0x5AU);
    std::memset(fake->Frame, 0, sizeof(fake->Frame));
    ASTERIA_EXPECT(context, "maximum READ10", storage.Read(1U, 128U, 128U, full, 65536U));
    Drive(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "maximum read complete", result.Status == M::StorageStatus::Ok,
                   std::memcmp(fake->Frame, fake->Disk + 65536U, 65536U) == 0);
    fake->FailOpcode = 0x28U;
    fake->FailKey = 6U;
    fake->FailAsc = 0x28U;
    ASTERIA_EXPECT(context, "medium change during IO", storage.Read(1U, 0U, 1U, Buffer, 512U));
    Drive(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "IO attention invalidates", result.Status == M::StorageStatus::MediumChanged,
                   result.Blocks == 0U, result.Generation == 2U, !storage.HasDescription(),
                   !storage.Read(1U, 0U, 1U, Buffer, 512U));
    fake->FailOpcode = 0xFFU;
    fake->ReadOnly = true;
    Describe(context, storage, *fake, result, now);
    const auto issued = fake->Commands[0x2AU];
    ASTERIA_EXPECT(context, "write protect refusal before issue", result.Description.ReadOnly,
                   !storage.Write(2U, 0U, 1U, Buffer, 512U), fake->Commands[0x2AU] == issued);
    fake->FailOpcode = 0x35U;
    fake->FailKey = 3U;
    ASTERIA_EXPECT(context, "Flush medium error", storage.Flush(2U));
    Drive(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "Flush other sense not swallowed", result.Status == M::StorageStatus::IoError);
    fake->FailOpcode = 0xFFU;
    fake->BlockSize = 4096U;
    Describe(context, storage, *fake, result, now);
    ASTERIA_EXPECT(context, "failed Describe keeps old snapshot", result.Status == M::StorageStatus::Unsupported,
                   storage.HasDescription(), storage.Read(2U, 0U, 1U, Buffer, 512U));
    Drive(context, storage, *fake, result, now);
}
void Deadlines(TestContext& context)
{
    for (unsigned variant{0U}; variant < 3U; ++variant)
    {
        auto fake = std::make_unique<FakeBot>();
        M::MassStorage storage{Binding};
        M::StorageResult result{};
        M::UInt64 now{0U};
        if (variant != 0U)
        {
            Describe(context, storage, *fake, result, now);
            ASTERIA_EXPECT(context, "deadline Read accepted", storage.Read(1U, 0U, 1U, Buffer, 512U));
        }
        else
        {
            ASTERIA_EXPECT(context, "deadline Describe accepted", storage.Describe());
        }
        storage.Step(*fake, now);
        const M::UInt64 token = fake->Pending.Token;
        if (variant != 0U)
        {
            const auto cbw = fake->Reply();
            ASTERIA_EXPECT(context, "deadline CBW", storage.Complete(cbw));
            storage.Step(*fake, now);
        }
        now += 5000U;
        fake->AbortRefused = variant == 2U;
        storage.Step(*fake, now);
        if (variant == 2U)
        {
            storage.Step(*fake, now);
            storage.Step(*fake, now);
            ASTERIA_EXPECT(context, "refused deadline abort faults with borrow retained", storage.IsFaulted(),
                           storage.IsOutstanding(), fake->Live == 1U, !storage.TakeResult(result));
            fake->AbortRefused = false;
            ASTERIA_EXPECT(context, "component can retry abort after fault", storage.Cancel(*fake));
        }
        ASTERIA_EXPECT(context, "policy deadline owns pending buffer", storage.IsOutstanding(), fake->Aborts == 1U,
                       fake->Live == 1U, !storage.TakeResult(result));
        const auto cancelled = fake->Reply();
        ASTERIA_EXPECT(context, "deadline completion", storage.Complete(cancelled),
                       (variant != 0U) || (cancelled.Token == token));
        Drive(context, storage, *fake, result, now);
        ASTERIA_EXPECT(context, "deadline result", result.Status == M::StorageStatus::Cancelled,
                       result.Generation == ((variant == 0U) ? 1U : 2U), fake->Resets == ((variant == 0U) ? 0U : 1U));
    }
    M::MassStorage invalid{Binding, 0U};
    ASTERIA_EXPECT(context, "zero deadline refused", invalid.IsFaulted(), !invalid.Describe());
}
void Model(TestContext& context)
{
    const auto range = Asteria::TestSupport::GetSeedRange(0xB070U, 16U);
    for (M::UInt64 run{0U}; run < range.Count; ++run)
    {
        const M::UInt64 seed = range.First + run;
        Asteria::TestSupport::Random random{seed};
        auto fake = std::make_unique<FakeBot>();
        M::MassStorage storage{Binding};
        M::StorageResult result{};
        M::UInt64 now{0U};
        M::UInt64 generation{1U};
        bool valid{false};
        M::UInt8 diskValue{0U};
        for (unsigned step{0U}; step < 80U; ++step)
        {
            const unsigned op = static_cast<unsigned>(random.Below(7U));
            fake->BadTag = false;
            fake->PhaseError = false;
            fake->ShortBy = 0U;
            fake->ExtraResidue = 0U;
            fake->FlushRefused = false;
            fake->FaultOpcode = 0x28U;
            if (!valid || op == 0U)
            {
                fake->Attention = (op == 0U) ? 0x28U : 0U;
                if (op == 0U)
                {
                    ++generation;
                }
                Describe(context, storage, *fake, result, now);
                valid = true;
            }
            else if (op == 1U)
            {
                fake->FlushRefused = true;
                ASTERIA_EXPECT(context, "model Flush", storage.Flush(generation));
                Drive(context, storage, *fake, result, now);
                if (!ASTERIA_EXPECT(context, "illegal-request Flush succeeds", result.Status == M::StorageStatus::Ok,
                                    result.Blocks == 0U, result.Description.BlockCount == 256U))
                {
                    std::fprintf(stderr, "seed=%llu step=%u operation=%u\n", seed, step, op);
                    return;
                }
            }
            else
            {
                fake->BadTag = op == 4U;
                fake->PhaseError = op == 5U;
                fake->ShortBy = (op == 6U) ? 512U : 0U;
                const bool write = op == 3U;
                fake->FaultOpcode = write ? 0x2AU : 0x28U;
                for (unsigned i{0U}; i < 512U; ++i)
                {
                    fake->Frame[512U + i] = static_cast<M::UInt8>(step);
                }
                ASTERIA_EXPECT(context, "model range",
                               write ? storage.Write(generation, 4U, 1U, Buffer, 512U)
                                     : storage.Read(generation, 4U, 1U, Buffer, 512U));
                Drive(context, storage, *fake, result, now);
                if (op == 4U || op == 5U)
                {
                    ++generation;
                    valid = false;
                }
                const M::StorageStatus expected = (op == 4U || op == 5U)
                                                      ? M::StorageStatus::TransportError
                                                      : ((op == 6U) ? M::StorageStatus::IoError : M::StorageStatus::Ok);
                if (!ASTERIA_EXPECT(context, "seeded result model", result.Status == expected,
                                    result.Blocks == ((expected == M::StorageStatus::Ok) ? 1U : 0U)))
                {
                    std::fprintf(stderr, "seed=%llu step=%u operation=%u\n", seed, step, op);
                    return;
                }
                if (write)
                {
                    diskValue = static_cast<M::UInt8>(step);
                }
                for (M::Size i{0U}; i < 512U; ++i)
                {
                    ASTERIA_EXPECT(context, "independent media model", fake->Disk[2048U + i] == diskValue);
                    if (!write && (op != 6U))
                    {
                        ASTERIA_EXPECT(context, "independent Frame model", fake->Frame[512U + i] == diskValue);
                    }
                }
            }
            if (!ASTERIA_EXPECT(context, "seeded state and resource model", storage.Generation() == generation,
                                storage.HasDescription() == valid, fake->Live == 0U, fake->MaxLive == 1U, fake->Valid))
            {
                std::fprintf(stderr, "seed=%llu step=%u operation=%u\n", seed, step, op);
                return;
            }
        }
    }
}
} // namespace
int main()
{
    TestContext context{"USB mass storage"};
    Discovery(context);
    Errors(context);
    CancelAndBounds(context);
    FacadeSemantics(context);
    Deadlines(context);
    Model(context);
    return context.Finish();
}
