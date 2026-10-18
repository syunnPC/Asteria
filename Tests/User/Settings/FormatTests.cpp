// SPDX-License-Identifier: Apache-2.0
#include "User/Settings/Database.hpp"
#include "Foundation/Crc32c.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
namespace S = Asteria::User::Settings;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::Size;
S::Snapshot state;
S::Snapshot decoded;
S::Snapshot before;
UInt8 original[S::SlotBytes];
UInt8 bytes[S::SlotBytes];
void Put(UInt8* p, const Size count, const UInt64 value)
{
    for (Size i{0U}; i < count; ++i)
    {
        p[i] = static_cast<UInt8>(value >> (i * 8U));
    }
}
void Repair()
{
    Put(bytes + 28U, 4U, 0U);
    const auto crc = Asteria::Foundation::Crc32c::Compute(bytes, S::BodyBytes);
    Put(bytes + 28U, 4U, crc);
    Put(bytes + S::SealOffset + 16U, 4U, crc);
    Put(bytes + S::SealOffset + 24U, 4U, Asteria::Foundation::Crc32c::Compute(bytes + S::SealOffset, 24U));
}
int main()
{
    Asteria::TestSupport::TestContext t{"SettingsFormatTests"};
    state.Keys[0U].Used = true;
    state.Keys[0U].AclCount = 1U;
    state.Keys[0U].Acl[0U] = {1U, S::AllRights, false, true};
    state.Keys[1U].Used = true;
    state.Keys[1U].NameLength = 1U;
    state.Keys[1U].Name[0U] = 'a';
    state.Keys[2U].Used = true;
    state.Keys[2U].NameLength = 1U;
    state.Keys[2U].Name[0U] = 'b';
    state.Values[0U].Used = true;
    state.Values[0U].KeyIndex = 1U;
    state.Values[0U].Data.Type = S::ValueType::Integer;
    state.Values[0U].Data.Length = 8U;
    state.Values[1U].Used = true;
    state.Values[1U].KeyIndex = 1U;
    state.Values[1U].NameLength = 1U;
    state.Values[1U].Name[0U] = 'x';
    state.Values[1U].Data.Type = S::ValueType::Boolean;
    state.Values[1U].Data.Length = 1U;
    UInt64 generation{99U};
    ASTERIA_EXPECT(t, "format has canonical roundtrip",
                   S::FormatCodec::Encode(state, 7U, 0U, original, sizeof(original)),
                   S::FormatCodec::Decode(original, sizeof(original), 0U, decoded, generation), generation == 7U);
    before = decoded;
    struct Mutation
    {
        Size Offset;
        UInt8 Byte;
    };
    // Offsets follow the version-2 table in SettingsDatabase.md. Repair both
    // checksums, so each refusal must come from identity/semantic validation.
    const Size root = S::HeaderBytes;
    const Size first = root + S::KeyRecordBytes;
    const Size second = first + S::KeyRecordBytes;
    const Size acl = root + 8U + S::MaximumNameBytes;
    const Mutation bad[]{{0U, 0U},
                         {8U, 1U},
                         {12U, 1U},
                         {16U, 0U},
                         {24U, 1U},
                         {root, 0U},
                         {root, 2U},
                         {root + 1U, 1U},
                         {root + 2U, 1U},
                         {root + 7U, 1U},
                         {first + 1U, 1U},   // cycle
                         {first + 1U, 3U},   // unused parent
                         {first + 1U, 255U}, // out-of-range parent
                         {first + 2U, 0U},
                         {first + 2U, 64U},
                         {first + 3U, 9U},
                         {first + 4U, 1U},
                         {first + 5U, 2U},
                         {first + 6U, 9U},
                         {first + 8U, 0U},
                         {first + 8U, '/'},
                         {first + 8U, '.'},
                         {first + 8U, 0xC0U},
                         {first + 9U, 1U},
                         {second + 8U, 'a'}, // duplicate sibling
                         {acl + 8U, 0U},
                         {acl + 8U, 32U},
                         {acl + 9U, 2U},
                         {acl + 10U, 2U},
                         {acl + 11U, 1U},
                         {acl + S::AclRecordBytes, 1U},            // unused ACL entry
                         {root + 3U * S::KeyRecordBytes + 8U, 1U}, // unused key data
                         {S::SealOffset, 0U},
                         {S::SealOffset + 8U, 8U},
                         {S::SealOffset + 20U, 1U},
                         {S::SealOffset + 28U, 1U}};
    const Size v = S::ValueTableOffset;
    const Size w = v + S::ValueRecordBytes;
    const Mutation badValues[]{{v, 0U},
                               {v, 2U},
                               {v + 1U, 3U},
                               {v + 1U, 255U},
                               {v + 2U, 64U},
                               {v + 3U, 6U},
                               {v + 4U, 7U},
                               {v + 5U, 2U},
                               {v + 6U, 1U},
                               {v + 7U, 1U},
                               {v + 8U, 1U},
                               {v + 8U + S::MaximumNameBytes + 8U, 1U},
                               {w + 8U, 0U},
                               {w + 8U, 0xC0U},
                               {w + 9U, 1U},
                               {w + 8U + S::MaximumNameBytes, 2U},
                               {w + S::ValueRecordBytes + 8U, 1U}};
    for (const auto mutation : badValues)
    {
        std::memcpy(bytes, original, sizeof(bytes));
        bytes[mutation.Offset] = mutation.Byte;
        Repair();
        generation = 99U;
        ASTERIA_EXPECT(t, "checksummed malformed named value refused atomically",
                       !S::FormatCodec::Decode(bytes, sizeof(bytes), 0U, decoded, generation), generation == 99U,
                       std::memcmp(&decoded, &before, sizeof(decoded)) == 0);
    }
    std::memcpy(bytes, original, sizeof(bytes));
    std::memcpy(bytes + w, bytes + v, S::ValueRecordBytes);
    Repair();
    ASTERIA_EXPECT(t, "duplicate default value name refused",
                   !S::FormatCodec::Decode(bytes, sizeof(bytes), 0U, decoded, generation));
    for (const auto mutation : bad)
    {
        std::memcpy(bytes, original, sizeof(bytes));
        bytes[mutation.Offset] = mutation.Byte;
        Repair();
        generation = 99U;
        ASTERIA_EXPECT(t, "checksummed malformed slot refused atomically",
                       !S::FormatCodec::Decode(bytes, sizeof(bytes), 0U, decoded, generation), generation == 99U,
                       std::memcmp(&decoded, &before, sizeof(decoded)) == 0);
    }
    std::memcpy(bytes, original, sizeof(bytes));
    for (Size i{2U}; i <= S::ValuesPerKey; ++i)
    {
        UInt8* record = bytes + S::ValueTableOffset + i * S::ValueRecordBytes;
        record[0U] = 1U;
        record[1U] = 1U;
        record[2U] = 1U;
        record[8U] = static_cast<UInt8>('a' + i);
    }
    Repair();
    ASTERIA_EXPECT(t, "checksummed per-key value overflow refused",
                   !S::FormatCodec::Decode(bytes, sizeof(bytes), 0U, decoded, generation));
    std::memcpy(bytes, original, sizeof(bytes));
    bytes[S::BodyBytes] = 1U;
    ASTERIA_EXPECT(t, "unallocated gap has no semantic meaning",
                   S::FormatCodec::Decode(bytes, sizeof(bytes), 0U, decoded, generation));
    ASTERIA_EXPECT(t, "exact slot size and slot identity required",
                   !S::FormatCodec::Decode(bytes, sizeof(bytes) - 1U, 0U, decoded, generation),
                   !S::FormatCodec::Decode(bytes, sizeof(bytes), 1U, decoded, generation),
                   !S::FormatCodec::Decode(nullptr, sizeof(bytes), 0U, decoded, generation));
    for (Size i{3U}; i <= S::MaximumDepth + 1U; ++i)
    {
        state.Keys[i].Used = true;
        state.Keys[i].Parent = static_cast<UInt8>(i - 1U);
        state.Keys[i].NameLength = 1U;
        state.Keys[i].Name[0U] = 'z';
    }
    // first and second are root children; chain beginning at second has nine
    // edges once key ten is added, which must be rejected even with valid CRCs.
    state.Keys[S::MaximumDepth + 2U].Used = true;
    state.Keys[S::MaximumDepth + 2U].Parent = static_cast<UInt8>(S::MaximumDepth + 1U);
    state.Keys[S::MaximumDepth + 2U].NameLength = 1U;
    state.Keys[S::MaximumDepth + 2U].Name[0U] = 'z';
    ASTERIA_EXPECT(t, "encoder refuses excessive depth", !S::FormatCodec::Encode(state, 1U, 0U, bytes, sizeof(bytes)));
    const UInt8 truncatedList[]{1U, 0U, 1U, 0U};
    const UInt8 emptyList[]{0U, 0U};
    const UInt8 badBool[]{2U};
    const UInt8 surrogate[]{0xEDU, 0xA0U, 0x80U};
    ASTERIA_EXPECT(t, "typed representation bounds", !S::FormatCodec::ValidValue(S::ValueType::Boolean, badBool, 1U),
                   !S::FormatCodec::ValidValue(S::ValueType::Integer, badBool, 1U),
                   !S::FormatCodec::ValidValue(S::ValueType::String, surrogate, 3U),
                   !S::FormatCodec::ValidValue(S::ValueType::StringList, truncatedList, sizeof(truncatedList)),
                   S::FormatCodec::ValidValue(S::ValueType::StringList, emptyList, sizeof(emptyList)),
                   !S::FormatCodec::ValidValue(S::ValueType::Binary, bytes, S::MaximumValueBytes + 1U));
    return t.Finish();
}
