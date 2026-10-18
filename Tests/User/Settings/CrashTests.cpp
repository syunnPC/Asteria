// SPDX-License-Identifier: Apache-2.0
#include "User/Settings/MemoryStorage.hpp"
#include "Support/TestContext.hpp"
#include "Foundation/Crc32c.hpp"
#include <cstring>
#include <initializer_list>
namespace S = Asteria::User::Settings;
namespace H = Asteria::TestSupport::Settings;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
H::MemoryStorage medium;
S::Database db;
UInt8 baseline[S::StorageBytes];
S::Value oldValue;
S::Value newValue;
S::Value actual;
inline constexpr UInt64 BaselineGeneration{5U};
S::Change event;
S::Snapshot parsed;
S::Snapshot before;
UInt8 slot[S::SlotBytes];
S::Status Mutate(const Size kind)
{
    switch (kind)
    {
    case 0U:
        return db.Write(H::Owner, H::View("/a"), H::View("data"), newValue);
    case 1U:
        return db.Create(H::Owner, H::View("/b"));
    case 2U:
        return db.Delete(H::Owner, H::View("/a"));
    case 4U:
        return db.Write(H::Owner, H::View("/a"), H::View("日"), newValue);
    case 5U:
        return db.DeleteValue(H::Owner, H::View("/a"), H::View("data"));
    default:
    {
        const S::AclEntry ace[2U]{H::RootAcl, {2U, S::ReadRight, false, true}};
        return db.SetAcl(H::Owner, H::View("/a"), ace, 2U);
    }
    }
}
bool Recovered(const Size kind)
{
    if (db.Open(medium.Interface()) != S::Status::Ok)
    {
        return false;
    }
    const auto generation = db.Generation();
    if ((generation != BaselineGeneration) && (generation != BaselineGeneration + 1U))
    {
        return false;
    }
    const bool committed = generation == BaselineGeneration + 1U;
    if ((kind != 2U) || !committed)
    {
        if ((db.Read(H::Owner, H::View("/a"), H::View(""), actual) != S::Status::Ok) || !H::Equal(actual, oldValue) ||
            (db.Read(H::Owner, H::View("/a"), H::View("other"), actual) != S::Status::Ok) ||
            !H::Equal(actual, newValue))
        {
            return false;
        }
    }
    const auto result = db.Read(H::Owner, H::View("/a"), H::View("data"), actual);
    if (((kind == 2U) || (kind == 5U)) && committed)
    {
        return result == S::Status::NotFound;
    }
    if ((result != S::Status::Ok) || !H::Equal(actual, ((kind == 0U) && committed) ? newValue : oldValue))
    {
        return false;
    }
    if (kind == 1U)
    {
        S::AclEntry ace[S::AclCapacity]{};
        Size count{0U};
        return db.GetAcl(H::Owner, H::View("/b"), ace, S::AclCapacity, count) ==
               (committed ? S::Status::Ok : S::Status::NotFound);
    }
    if (kind == 4U)
    {
        return db.Read(H::Owner, H::View("/a"), H::View("日"), actual) ==
                   (committed ? S::Status::Ok : S::Status::NotFound) &&
               (!committed || H::Equal(actual, newValue));
    }
    if (kind == 3U)
    {
        S::AclEntry ace[S::AclCapacity]{};
        Size count{99U};
        return (db.GetAcl(H::Owner, H::View("/a"), ace, S::AclCapacity, count) == S::Status::Ok) &&
               (count == (committed ? 2U : 0U));
    }
    return true;
}
void Restore()
{
    medium.FailCall = 0U;
    medium.Calls = 0U;
    medium.Readable = S::StorageBytes;
    medium.CorruptWrite = false;
    medium.Capture = false;
    std::memcpy(medium.Bytes, baseline, sizeof(baseline));
}
int main()
{
    Asteria::TestSupport::TestContext t{"SettingsCrashTests"};
    oldValue.Type = S::ValueType::String;
    oldValue.Length = 3U;
    std::memcpy(oldValue.Bytes, "old", 3U);
    newValue.Type = S::ValueType::Binary;
    newValue.Length = S::MaximumValueBytes;
    for (Size i{0U}; i < newValue.Length; ++i)
    {
        newValue.Bytes[i] = static_cast<UInt8>(i);
    }
    ASTERIA_EXPECT(t, "baseline with older sealed inactive slot",
                   db.Format(medium.Interface(), &H::RootAcl, 1U) == S::Status::Ok,
                   db.Create(H::Owner, H::View("/a")) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/a"), H::View("data"), oldValue) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/a"), H::View(""), oldValue) == S::Status::Ok,
                   db.Write(H::Owner, H::View("/a"), H::View("other"), newValue) == S::Status::Ok,
                   db.Generation() == BaselineGeneration);
    std::memcpy(baseline, medium.Bytes, sizeof(baseline));
    for (Size kind{0U}; kind < 6U; ++kind)
    {
        Restore();
        bool good = db.Open(medium.Interface()) == S::Status::Ok;
        medium.Calls = 0U;
        medium.Capture = true;
        good &= Mutate(kind) == S::Status::Ok;
        good &= medium.Calls == 6U;
        // Crash after every completed write and every completed flush.
        for (Size call{0U}; call < 6U; ++call)
        {
            std::memcpy(medium.Bytes, medium.Boundaries[call], S::StorageBytes);
            good &= Recovered(kind);
            good &= db.Generation() == (call < 4U ? BaselineGeneration : BaselineGeneration + 1U);
            const Size offset = medium.Offsets[call];
            const Size length = medium.Lengths[call];
            // Truncate exactly at each write boundary; a slot read cannot
            // accept even a complete body without its full seal.
            if (length != 0U)
            {
                medium.Readable = offset + length;
                good &= Recovered(kind);
                medium.Readable = S::StorageBytes;
                medium.Bytes[offset + length - 1U] ^= 0x80U;
                good &= Recovered(kind);
                good &= db.Generation() == BaselineGeneration;
            }
        }
        ASTERIA_EXPECT(t, "truncate/corrupt and recover at each write/flush boundary", good);
        // Fail each callback, including final-flush uncertainty. The failed
        // instance is unusable; reopening must yield exactly old or new state.
        bool failures{true};
        for (Size call{1U}; call <= 6U; ++call)
        {
            const Size sizes[6U]{S::SealBytes, 0U, S::BodyBytes, 0U, S::SealBytes, 0U};
            const Size length = sizes[call - 1U];
            const Size prefixes[6U]{0U, 1U, length / 2U, length > 0U ? length - 1U : 0U, length, length + 1U};
            for (const auto prefix : prefixes)
            {
                for (const bool corrupt : {false, true})
                {
                    Restore();
                    failures &= db.Open(medium.Interface()) == S::Status::Ok;
                    UInt64 watch{0U};
                    failures &= db.Watch(H::Owner, H::View("/"), true, watch) == S::Status::Ok;
                    medium.Calls = 0U;
                    medium.FailCall = call;
                    medium.Prefix = prefix;
                    medium.CorruptWrite = corrupt;
                    failures &= Mutate(kind) == S::Status::IoError;
                    failures &= db.Generation() == 0U;
                    failures &= db.Poll(watch, event) == S::Status::Closed;
                    medium.FailCall = 0U;
                    failures &= Recovered(kind);
                }
            }
        }
        ASTERIA_EXPECT(t, "partial writes/corruption, failures and final-flush ambiguity", failures);
    }
    bool tears{true};
    // Every possible seal tear, plus every record boundary in the large body.
    for (Size call : {1U, 3U, 5U})
    {
        const Size limit = call == 3U ? S::BodyBytes : S::SealBytes;
        for (Size prefix{0U}; prefix <= limit; ++prefix)
        {
            if ((call == 3U) && (prefix != 0U) && (prefix != limit) && (prefix != S::HeaderBytes) &&
                ((prefix < S::HeaderBytes) ||
                 ((prefix < S::ValueTableOffset) ? ((prefix - S::HeaderBytes) % S::KeyRecordBytes != 0U)
                                                 : ((prefix - S::ValueTableOffset) % S::ValueRecordBytes != 0U))))
            {
                continue;
            }
            Restore();
            tears &= db.Open(medium.Interface()) == S::Status::Ok;
            medium.Calls = 0U;
            medium.FailCall = call;
            medium.Prefix = prefix;
            tears &= Mutate(0U) == S::Status::IoError;
            medium.FailCall = 0U;
            tears &= Recovered(0U);
        }
    }
    ASTERIA_EXPECT(t, "exhaustive seal tears and key-record body boundaries", tears);
    Restore();
    std::memcpy(slot, medium.Bytes, S::SlotBytes);
    UInt64 generation{99U};
    ASTERIA_EXPECT(t, "committed parser roundtrip", S::FormatCodec::Decode(slot, S::SlotBytes, 0U, parsed, generation),
                   generation == BaselineGeneration);
    before = parsed;
    slot[S::HeaderBytes] ^= 1U;
    ASTERIA_EXPECT(t, "parser rejection leaves output unchanged",
                   !S::FormatCodec::Decode(slot, S::SlotBytes, 0U, parsed, generation),
                   generation == BaselineGeneration, std::memcmp(&parsed, &before, sizeof(parsed)) == 0);
    Restore();
    medium.Bytes[S::SealOffset] ^= 1U;
    ASTERIA_EXPECT(t, "corrupt newest falls back to older committed slot", db.Open(medium.Interface()) == S::Status::Ok,
                   db.Generation() == BaselineGeneration - 1U);
    medium.Bytes[S::SlotBytes + S::SealOffset] ^= 1U;
    ASTERIA_EXPECT(t, "both corrupt refused without formatting", db.Open(medium.Interface()) == S::Status::Corrupt,
                   db.Generation() == 0U);
    Restore();
    medium.Readable = S::SlotBytes - 1U;
    ASTERIA_EXPECT(t, "truncated medium refused", db.Open(medium.Interface()) == S::Status::Corrupt);
    // No wrap or ABA at the generation limit.
    Restore();
    generation = ~UInt64{0U};
    ASTERIA_EXPECT(t, "max generation can be read",
                   S::FormatCodec::Encode(before, generation, 0U, medium.Bytes, S::SlotBytes),
                   db.Open(medium.Interface()) == S::Status::Ok);
    ASTERIA_EXPECT(t, "generation exhaustion fails without writes",
                   db.Write(H::Owner, H::View("/a"), H::View("data"), newValue) == S::Status::Exhausted,
                   db.Generation() == generation);
    return t.Finish();
}
