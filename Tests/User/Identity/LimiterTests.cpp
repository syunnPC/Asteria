// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/Support.hpp"
#include <map>
using Asteria::User::Identity::Account;
using Asteria::User::Identity::AccountCapacity;
using Asteria::User::Identity::Administrator;
using Asteria::User::Identity::AttemptLimiter;
using Asteria::User::Identity::Bucket;
using Asteria::User::Identity::LimiterState;
using Asteria::User::Identity::MaximumCounter;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::Status;
using IdentityTest::Time;
using Asteria::User::Identity::TimeTicksPerSecond;
using Asteria::User::Identity::UInt64;
struct ModelBucket
{
    UInt64 Fail{0U};
    UInt64 Last{0U};
    UInt64 Until{0U};
};
struct Model
{
    std::map<UInt64, ModelBucket> Buckets;
    UInt64 Global{0U};
    UInt64 Serial{1U};
    UInt64 Live{0U};
    UInt64 Account{0U};
    Status Begin(UInt64 id, UInt64 now, UInt64& serial, UInt64& retry)
    {
        if (Live != 0U)
        {
            return Status::Busy;
        }
        auto& b = Buckets[id];
        if (b.Fail && now - b.Last >= 900U)
        {
            b.Fail = 0U;
            b.Until = 0U;
        }
        const auto deadline = std::max(Global, b.Until);
        if (now < deadline)
        {
            retry = deadline;
            return Status::RetryAt;
        }
        Account = id;
        Live = Serial++;
        serial = Live;
        return Status::Ok;
    }
    Status Complete(UInt64 serial, UInt64 now, bool ok, bool cancel)
    {
        if (Live == 0U || serial != Live)
        {
            return Status::Invalid;
        }
        auto& b = Buckets[Account];
        if (ok && !cancel)
        {
            b.Fail = 0U;
            b.Until = 0U;
        }
        else
        {
            ++b.Fail;
            b.Last = now;
            const UInt64 delays[]{1U, 2U, 4U, 8U};
            b.Until = now + (b.Fail <= 4U ? delays[b.Fail - 1U] : (b.Fail < 10U ? 30U : 300U));
        }
        Global = now + 1U;
        Live = 0U;
        return Status::Ok;
    }
};
static LimiterState State;
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"IdentityLimiter"};
    Time time;
    AttemptLimiter limiter(State, time.Interface());
    UInt64 serial = 88U;
    UInt64 retry = 99U;
    const UInt64 delays[]{1U, 2U, 4U, 8U, 30U, 30U, 30U, 30U, 30U, 300U, 300U};
    bool boundary = true;
    for (Size f = 0U; f < 11U; ++f)
    {
        boundary &= limiter.Begin(Administrator, serial, retry) == Status::Ok;
        boundary &= limiter.Complete(serial, false) == Status::Ok;
        const auto completed = time.Seconds;
        time.Seconds = completed + delays[f] - 1U;
        const auto before = State.NextSerial;
        boundary &= limiter.Begin(Administrator, serial, retry) == Status::RetryAt &&
                    retry == (completed + delays[f]) * TimeTicksPerSecond && State.NextSerial == before;
        time.Seconds = completed + delays[f];
    }
    ASTERIA_EXPECT(t, "every throttle boundary inclusive expiry", boundary);
    ASTERIA_EXPECT(t, "success resets only account", limiter.Begin(Administrator, serial, retry) == Status::Ok,
                   limiter.Complete(serial, true) == Status::Ok);
    ASTERIA_EXPECT(t, "global one second after success", limiter.Begin(0U, serial, retry) == Status::RetryAt,
                   retry == (time.Seconds + 1U) * TimeTicksPerSecond);
    ++time.Seconds;
    ASTERIA_EXPECT(t, "fixed unknown bucket", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, false) == Status::Ok, State.Unknown.Failures == 1U);
    ++time.Seconds;
    ASTERIA_EXPECT(t, "cancel after began is failure", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, true, true) == Status::Ok, State.Unknown.Failures == 2U);
    time.Seconds += 2U;
    ASTERIA_EXPECT(t, "accepted checkpoint", limiter.Begin(0U, serial, retry) == Status::Ok, State.InFlight);
    AttemptLimiter restarted(State, time.Interface());
    ASTERIA_EXPECT(t, "restart counts inflight failed", restarted.Recover() == Status::Ok, State.Unknown.Failures == 3U,
                   !State.InFlight);
    time.Seconds = State.Unknown.LastFailure / TimeTicksPerSecond + 899U;
    ASTERIA_EXPECT(t, "899 seconds preserves failures", restarted.Begin(0U, serial, retry) == Status::Ok,
                   State.Unknown.Failures == 3U, restarted.Complete(serial, false) == Status::Ok);
    time.Seconds = State.Unknown.LastFailure / TimeTicksPerSecond + 900U;
    ASTERIA_EXPECT(t, "900 seconds clears failures", restarted.Begin(0U, serial, retry) == Status::Ok,
                   State.Unknown.Failures == 0U, restarted.Complete(serial, true) == Status::Ok);
    time.Available = false;
    serial = 88U;
    retry = 99U;
    ASTERIA_EXPECT(t, "missing clock preserves outputs", restarted.Begin(0U, serial, retry) == Status::Unavailable,
                   serial == 88U, retry == 99U);
    time.Available = true;
    --time.Seconds;
    ASTERIA_EXPECT(t, "backward clock", restarted.Begin(0U, serial, retry) == Status::Unavailable);
    time.Seconds = State.LastTime / TimeTicksPerSecond + 1U;
    ASTERIA_EXPECT(t, "clock recovers", restarted.Begin(0U, serial, retry) == Status::Ok);
    time.Available = false;
    ASTERIA_EXPECT(t, "terminal unavailable retains checkpoint",
                   restarted.Complete(serial, true) == Status::Unavailable, State.InFlight);
    time.Available = true;
    ASTERIA_EXPECT(t, "restart after unknown terminal", restarted.Recover() == Status::Ok,
                   State.Unknown.Failures == 1U);
    State = {};
    time.RawTicks = true;
    time.Seconds = TimeTicksPerSecond - 1U;
    ASTERIA_EXPECT(t, "subsecond completion retains full one second", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, false) == Status::Ok);
    time.Seconds = 2U * TimeTicksPerSecond - 2U;
    ASTERIA_EXPECT(t, "one nanosecond before deadline refuses", limiter.Begin(0U, serial, retry) == Status::RetryAt,
                   retry == 2U * TimeTicksPerSecond - 1U);
    ++time.Seconds;
    ASTERIA_EXPECT(t, "exact nanosecond boundary", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, true) == Status::Ok);
    State = {};
    time.RawTicks = false;
    time.Seconds = 0U;
    Model model;
    UInt64 random = 1234567U;
    bool matches = true;
    for (Size step = 0U; step < 5000U; ++step)
    {
        random = random * 6364136223846793005ULL + 1U;
        time.Seconds += (random >> 60U);
        const UInt64 id = (random & 3U) == 0U ? 0U : 256U + (random % 8U);
        UInt64 aSerial = 77U;
        UInt64 bSerial = 77U;
        UInt64 aRetry = 99U;
        UInt64 bRetry = 99U;
        auto a = limiter.Begin(id, aSerial, aRetry);
        auto b = model.Begin(id, time.Seconds, bSerial, bRetry);
        matches &= a == b && aSerial == bSerial && (aRetry == bRetry || aRetry == bRetry * TimeTicksPerSecond);
        if (a == Status::Ok)
        {
            time.Seconds += (random >> 61U);
            bool success = (random & 16U) != 0U;
            bool cancel = (random & 128U) != 0U;
            matches &=
                limiter.Complete(aSerial, success, cancel) == model.Complete(bSerial, time.Seconds, success, cancel);
        }
        matches &= State.GlobalDeadline == model.Global * TimeTicksPerSecond;
        for (const auto& pair : model.Buckets)
        {
            const Bucket empty{};
            const Bucket* actual = pair.first == 0U ? &State.Unknown : &empty;
            for (const auto& bucket : State.Accounts)
            {
                if (bucket.Id == pair.first && pair.first != 0U)
                {
                    actual = &bucket;
                }
            }
            matches &= actual->Failures == pair.second.Fail &&
                       actual->Deadline == pair.second.Until * TimeTicksPerSecond &&
                       actual->LastFailure == pair.second.Last * TimeTicksPerSecond;
        }
    }
    ASTERIA_EXPECT(t, "5000 independent model transitions", matches);
    State = {};
    State.Unknown.Failures = MaximumCounter;
    State.Unknown.LastFailure = time.Seconds * TimeTicksPerSecond;
    ASTERIA_EXPECT(t, "counter saturation", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, false) == Status::Ok, State.Unknown.Failures == MaximumCounter);
    State = {};
    State.NextSerial = MaximumCounter;
    serial = 88U;
    ASTERIA_EXPECT(t, "serial never wraps", limiter.Begin(0U, serial, retry) == Status::Exhausted, serial == 88U);
    State = {};
    time.RawTicks = true;
    time.Seconds = MaximumCounter - TimeTicksPerSecond;
    ASTERIA_EXPECT(t, "deadline overflow blocks rather than wraps", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, false) == Status::Ok);
    time.Seconds = MaximumCounter;
    ASTERIA_EXPECT(t, "terminal time exhaustion", limiter.Begin(0U, serial, retry) == Status::Ok,
                   limiter.Complete(serial, false) == Status::Exhausted,
                   limiter.Begin(0U, serial, retry) == Status::Exhausted);
    State = {};
    time.RawTicks = false;
    time.Seconds = 0U;
    bool full = true;
    for (Size i = 0; i < AccountCapacity; ++i)
    {
        full &= limiter.Begin(256U + i, serial, retry) == Status::Ok && limiter.Complete(serial, true) == Status::Ok;
        ++time.Seconds;
    }
    ASTERIA_EXPECT(t, "bounded buckets no eviction", full, limiter.Begin(999U, serial, retry) == Status::Full,
                   limiter.Begin(0U, serial, retry) == Status::Ok);
    return t.Finish();
}
