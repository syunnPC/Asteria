// SPDX-License-Identifier: Apache-2.0
// The common driver bounds inputs to 4096 bytes. A two-byte offset plus a
// patch mutates a complete valid slot, allowing coverage of all disk fields.
// A second pass repairs both CRCs to exercise semantic rejection. Larger
// external inputs are also tested directly as complete/truncated slots.
#include "User/Settings/Database.hpp"
#include "Foundation/Crc32c.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace
{
namespace S = Asteria::User::Settings;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::Size;
UInt8 seed[S::SlotBytes];
UInt8 wire[S::SlotBytes];
UInt8 encoded[S::SlotBytes];
S::Snapshot model;
S::Snapshot output;
S::Snapshot before;
bool initialized{false};
void Require(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}
void Put(UInt8* p, const Size count, const UInt64 value)
{
    for (Size i{0U}; i < count; ++i)
    {
        p[i] = static_cast<UInt8>(value >> (i * 8U));
    }
}
void Init()
{
    if (initialized)
    {
        return;
    }
    model.Keys[0U].Used = true;
    model.Keys[0U].AclCount = 1U;
    model.Keys[0U].Acl[0U] = {1U, S::AllRights, false, true};
    model.Keys[1U].Used = true;
    model.Keys[1U].NameLength = 1U;
    model.Keys[1U].Name[0U] = 'a';
    model.Values[0U].Used = true;
    model.Values[0U].KeyIndex = 1U;
    model.Values[1U].Used = true;
    model.Values[1U].KeyIndex = 1U;
    model.Values[1U].NameLength = 1U;
    model.Values[1U].Name[0U] = 'b';
    model.Values[1U].Data.Type = S::ValueType::Boolean;
    model.Values[1U].Data.Length = 1U;
    model.Values[0U].Data.Type = S::ValueType::String;
    model.Values[0U].Data.Length = 3U;
    model.Values[0U].Data.Bytes[0U] = 0xE6U;
    model.Values[0U].Data.Bytes[1U] = 0x97U;
    model.Values[0U].Data.Bytes[2U] = 0xA5U;
    model.Keys[2U].Used = true;
    model.Keys[2U].Parent = 1U;
    model.Keys[2U].NameLength = 1U;
    model.Keys[2U].Name[0U] = 'b';
    Require(S::FormatCodec::Encode(model, 42U, 0U, seed, sizeof(seed)));
    initialized = true;
}
void Check(const UInt8* bytes, const Size size, const UInt8 slot)
{
    before = output;
    UInt64 generation{0xCAFEU};
    if (S::FormatCodec::Decode(bytes, size, slot, output, generation))
    {
        Require(S::FormatCodec::Encode(output, generation, slot, encoded, sizeof(encoded)));
        Require(std::memcmp(bytes, encoded, S::BodyBytes) == 0);
        Require(std::memcmp(bytes + S::SealOffset, encoded + S::SealOffset, S::SealBytes) == 0);
        UInt64 untouched{0xCAFEU};
        before = output;
        Require(!S::FormatCodec::Decode(bytes, size, static_cast<UInt8>(1U - slot), output, untouched));
        Require(untouched == 0xCAFEU);
        Require(std::memcmp(&output, &before, sizeof(output)) == 0);
    }
    else
    {
        Require(generation == 0xCAFEU);
        Require(std::memcmp(&output, &before, sizeof(output)) == 0);
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    Init();
    Check(data, size, 0U);
    Check(data, size, 1U);
    if (size < 2U)
    {
        return 0;
    }
    const Size at = (static_cast<Size>(data[0U]) | (static_cast<Size>(data[1U]) << 8U)) % S::SlotBytes;
    const Size patch = size - 2U < S::SlotBytes - at ? size - 2U : S::SlotBytes - at;
    std::memcpy(wire, seed, sizeof(wire));
    std::memcpy(wire + at, data + 2U, patch);
    Check(wire, sizeof(wire), 0U);
    UInt8* const seal = wire + S::SealOffset;
    Put(wire + 28U, 4U, 0U);
    const auto crc = Asteria::Foundation::Crc32c::Compute(wire, S::BodyBytes);
    Put(wire + 28U, 4U, crc);
    Put(seal + 16U, 4U, crc);
    // Keep the parser's header and seal identity cross-checks, including
    // mutated generation and slot, rather than silently repairing those.
    Put(seal + 24U, 4U, Asteria::Foundation::Crc32c::Compute(seal, 24U));
    Check(wire, sizeof(wire), 0U);
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    Init();
    UInt8 patch[2U + S::ValueRecordBytes]{};
    emit(patch, 2U);
    const Size offsets[]{0U,
                         8U,
                         12U,
                         16U,
                         24U,
                         28U,
                         S::HeaderBytes,
                         S::HeaderBytes + S::KeyRecordBytes,
                         S::HeaderBytes + 2U * S::KeyRecordBytes,
                         S::ValueTableOffset,
                         S::ValueTableOffset + S::ValueRecordBytes,
                         S::SealOffset};
    for (const auto at : offsets)
    {
        patch[0U] = static_cast<UInt8>(at);
        patch[1U] = static_cast<UInt8>(at >> 8U);
        const Size length =
            at >= S::SealOffset
                ? S::SealBytes
                : (at < S::HeaderBytes ? S::HeaderBytes
                                       : (at < S::ValueTableOffset ? S::KeyRecordBytes : S::ValueRecordBytes));
        std::memcpy(patch + 2U, seed + at, length);
        emit(patch, length + 2U);
    }
}
