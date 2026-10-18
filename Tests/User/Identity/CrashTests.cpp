// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/Support.hpp"
using Asteria::User::Identity::AccountInfo;
using Asteria::User::Identity::AccountStore;
using Asteria::User::Identity::Administrator;
using Asteria::User::Identity::FormatCodec;
using IdentityTest::Memory;
using Asteria::User::Identity::PrincipalId;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::SealOffset;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::SlotBytes;
using Asteria::User::Identity::Snapshot;
using Asteria::User::Identity::Status;
using Asteria::User::Identity::StorageBytes;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
using IdentityTest::View;
static Memory Disk;
static AccountStore Store;
static AccountStore Reopened;
static UInt8 Baseline[StorageBytes];
static UInt8 New[StorageBytes];
static Snapshot State;
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"IdentityCrash"};
    ASTERIA_EXPECT(t, "open absent fails closed", Store.Open(Disk.Interface()) == Status::Corrupt,
                   Store.Generation() == 0U);
    ASTERIA_EXPECT(t, "explicit format", Store.Format(Disk.Interface()) == Status::Ok,
                   Store.Ready() == Status::NotProvisioned);
    std::memcpy(Baseline, Disk.Bytes, StorageBytes);
    Disk.Calls = 0U;
    Disk.Capture = true;
    PrincipalId id = 77U;
    ASTERIA_EXPECT(t, "mutation", Store.CreateAccount(View("alice"), Role::Standard, id) == Status::Ok, id == 256U,
                   Disk.Calls == 6U);
    std::memcpy(New, Disk.Bytes, StorageBytes);
    bool boundaries = true;
    for (Size b = 0; b < 6U; ++b)
    {
        std::memcpy(Disk.Bytes, Disk.Trace[b], StorageBytes);
        AccountInfo info{};
        boundaries &= Reopened.Open(Disk.Interface()) == Status::Ok;
        const auto found = Reopened.FindAccount(View("alice"), info);
        boundaries &= (found == Status::NotFound && Reopened.Generation() == 1U) ||
                      (found == Status::Ok && info.Id == 256U && Reopened.Generation() == 2U);
        boundaries &= std::memcmp(Disk.Bytes, Baseline, SlotBytes) == 0;
    }
    ASTERIA_EXPECT(t, "crash after every write/flush reopen old-or-new", boundaries);
    Disk.Capture = false;
    bool tears = true;
    // Every boundary fails before/after a partial/full write; also power loss
    // discards unflushed bytes. No failed call may leave the store queryable.
    const Size prefixes[]{0U, 1U, 8U, 24U, 31U, 32U, 64U, 16448U, SealOffset};
    for (Size boundary = 1U; boundary <= 6U; ++boundary)
    {
        for (Size prefix : prefixes)
        {
            std::memcpy(Disk.Bytes, Baseline, StorageBytes);
            std::memcpy(Disk.Durable, Baseline, StorageBytes);
            Disk.Fail = 0U;
            tears &= Store.Open(Disk.Interface()) == Status::Ok;
            Disk.Calls = 0U;
            Disk.Fail = boundary;
            Disk.Prefix = prefix;
            id = 77U;
            tears &= Store.CreateAccount(View("alice"), Role::Standard, id) == Status::IoError &&
                     Store.Generation() == 0U && id == 77U;
            AccountInfo info{};
            tears &= Store.GetAccount(Administrator, info) == Status::Closed;
            for (Size power = 0; power < 2U; ++power)
            {
                if (power != 0U)
                {
                    std::memcpy(Disk.Bytes, Disk.Durable, StorageBytes);
                }
                Disk.Fail = 0U;
                tears &= Reopened.Open(Disk.Interface()) == Status::Ok;
                const auto found = Reopened.FindAccount(View("alice"), info);
                tears &= (found == Status::NotFound && Reopened.Generation() == 1U) ||
                         (found == Status::Ok && Reopened.Generation() == 2U);
                tears &= std::memcmp(Disk.Bytes, Baseline, SlotBytes) == 0;
            }
        }
    }
    ASTERIA_EXPECT(t, "all failed writes/flushes and torn prefixes", tears);
    Disk.Fail = 0U;
    std::memcpy(Disk.Bytes, New, StorageBytes);
    Disk.Bytes[SlotBytes + 100U] ^= 0x80U;
    ASTERIA_EXPECT(t, "one bad slot recovers", Store.Open(Disk.Interface()) == Status::Ok, Store.Generation() == 1U);
    Disk.Bytes[100U] ^= 0x80U;
    ASTERIA_EXPECT(t, "both bad fail closed", Store.Open(Disk.Interface()) == Status::Corrupt);
    std::memcpy(Disk.Bytes, Baseline, StorageBytes);
    UInt64 generation = 0U;
    ASTERIA_EXPECT(t, "decode baseline", FormatCodec::Decode(Disk.Bytes, SlotBytes, 0U, State, generation));
    ASTERIA_EXPECT(t, "equal generation fixture", FormatCodec::Encode(State, 1U, 1U, Disk.Bytes + SlotBytes, SlotBytes),
                   Store.Open(Disk.Interface()) == Status::Corrupt);
    ASTERIA_EXPECT(t, "format refuses nonzero", Store.Format(Disk.Interface()) == Status::Exists);
    Disk.Reset();
    Disk.FailRead = true;
    ASTERIA_EXPECT(t, "failed reads", Store.Open(Disk.Interface()) == Status::Corrupt,
                   Store.Format(Disk.Interface()) == Status::IoError);
    Disk.Reset();
    bool initial = true;
    for (Size b = 1U; b <= 6U; ++b)
    {
        Disk.Reset();
        Disk.Fail = b;
        Disk.Prefix = SealOffset;
        initial &= Store.Format(Disk.Interface()) == Status::IoError;
        Disk.Fail = 0U;
        auto opened = Store.Open(Disk.Interface());
        initial &= opened == Status::Ok || opened == Status::Corrupt;
    }
    ASTERIA_EXPECT(t, "format crash never manufactures fallback", initial);
    return t.Finish();
}
