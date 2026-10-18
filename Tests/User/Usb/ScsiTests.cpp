// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Scsi.hpp"

#include <cstring>

#include "Support/TestContext.hpp"

namespace M = Asteria::User::Usb;
int main()
{
    Asteria::TestSupport::TestContext context{"SCSI codecs"};
    M::UInt8 bytes[64U]{};
    M::Sense sense{};
    ASTERIA_EXPECT(context, "null sense", !M::Scsi::DecodeSense(nullptr, 64U, sense));
    bytes[0U] = 0x70U;
    bytes[2U] = 2U;
    bytes[7U] = 10U;
    bytes[12U] = 4U;
    bytes[13U] = 1U;
    ASTERIA_EXPECT(context, "fixed becoming ready", M::Scsi::DecodeSense(bytes, 18U, sense),
                   sense.Status == M::SenseStatus::BecomingReady);
    for (M::Size length{0U}; length < 18U; ++length)
    {
        ASTERIA_EXPECT(context, "sense declared length", !M::Scsi::DecodeSense(bytes, length, sense));
    }
    bytes[0U] = 0x71U;
    ASTERIA_EXPECT(context, "deferred sense", M::Scsi::DecodeSense(bytes, 18U, sense),
                   sense.Status == M::SenseStatus::Error);
    struct Mapping
    {
        M::UInt8 Key;
        M::UInt8 Asc;
        M::UInt8 Ascq;
        M::SenseStatus Status;
    };
    constexpr Mapping mappings[]{
        {0U, 0U, 0U, M::SenseStatus::Good},
        {2U, 4U, 1U, M::SenseStatus::BecomingReady},
        {2U, 4U, 2U, M::SenseStatus::NotReady},
        {2U, 0x3AU, 0U, M::SenseStatus::NoMedium},
        {6U, 0x28U, 0U, M::SenseStatus::MediumChanged},
        {6U, 0x29U, 0U, M::SenseStatus::Reset},
        {6U, 0x2AU, 0U, M::SenseStatus::Error},
        {5U, 0x20U, 0U, M::SenseStatus::IllegalRequest},
        {7U, 0x27U, 0U, M::SenseStatus::WriteProtected},
        {3U, 0U, 0U, M::SenseStatus::MediumError},
    };
    for (const Mapping& mapping : mappings)
    {
        std::memset(bytes, 0, 64U);
        bytes[0U] = 0x72U;
        bytes[1U] = mapping.Key;
        bytes[2U] = mapping.Asc;
        bytes[3U] = mapping.Ascq;
        ASTERIA_EXPECT(context, "sense status set", M::Scsi::DecodeSense(bytes, 8U, sense),
                       sense.Status == mapping.Status);
    }
    bytes[7U] = 3U;
    bytes[9U] = 2U;
    ASTERIA_EXPECT(context, "descriptor overrun", !M::Scsi::DecodeSense(bytes, 11U, sense));
    bytes[7U] = 1U;
    ASTERIA_EXPECT(context, "descriptor missing header", !M::Scsi::DecodeSense(bytes, 9U, sense));
    M::Capacity capacity{.BlockCount = 99U};
    std::memset(bytes, 255, 8U);
    bytes[4U] = 0U;
    bytes[5U] = 0U;
    bytes[6U] = 2U;
    bytes[7U] = 0U;
    ASTERIA_EXPECT(context, "capacity10 saturated", M::Scsi::Capacity10(bytes, 8U, capacity), capacity.Saturated,
                   capacity.BlockCount == 0x100000000ULL, capacity.BlockSize == 512U);
    std::memset(bytes, 255, 8U);
    bytes[8U] = 0U;
    bytes[9U] = 0U;
    bytes[10U] = 2U;
    bytes[11U] = 0U;
    const M::Capacity before = capacity;
    ASTERIA_EXPECT(context, "capacity16 overflow atomic", !M::Scsi::Capacity16(bytes, 32U, capacity),
                   capacity.BlockCount == before.BlockCount, capacity.BlockSize == before.BlockSize);
    bytes[7U] = 254U;
    ASTERIA_EXPECT(context, "capacity16 top usable", M::Scsi::Capacity16(bytes, 32U, capacity),
                   capacity.BlockCount == ~M::UInt64{0U});
    std::memset(bytes, 0, 64U);
    ASTERIA_EXPECT(context, "zero block sizes rejected", !M::Scsi::Capacity10(bytes, 8U, capacity),
                   !M::Scsi::Capacity16(bytes, 32U, capacity));
    bool readOnly{true};
    bytes[0U] = 2U;
    ASTERIA_EXPECT(context, "mode header length atomic", !M::Scsi::ModeSense(bytes, 4U, false, readOnly), readOnly);
    bytes[0U] = 63U;
    bytes[2U] = 0x80U;
    ASTERIA_EXPECT(context, "truncated mode pages accepted", M::Scsi::ModeSense(bytes, 4U, false, readOnly), readOnly);
    bytes[3U] = 61U;
    ASTERIA_EXPECT(context, "mode descriptor declared overrun", !M::Scsi::ModeSense(bytes, 4U, false, readOnly));
    M::Inquiry inquiry{};
    std::memset(bytes, 0, 64U);
    std::memset(bytes + 8U, ' ', 24U);
    bytes[4U] = 31U;
    ASTERIA_EXPECT(context, "inquiry exact minimum", M::Scsi::DecodeInquiry(bytes, 36U, inquiry));
    bytes[0U] = 0x20U;
    ASTERIA_EXPECT(context, "unsupported qualifier", !M::Scsi::DecodeInquiry(bytes, 36U, inquiry));
    bytes[0U] = 0U;
    bytes[8U] = 0U;
    ASTERIA_EXPECT(context, "inquiry string validation", !M::Scsi::DecodeInquiry(bytes, 36U, inquiry));
    M::ScsiCommand command{};
    const M::FrameBuffer buffer{.Length = 65536U};
    ASTERIA_EXPECT(context, "READ10 endpoint range",
                   M::Scsi::RangeCommand(false, 0xFFFFFFFFU, 1U, buffer, 512U, command), command.Bytes[2U] == 255U,
                   command.Bytes[5U] == 255U, !M::Scsi::RangeCommand(false, 0xFFFFFFFFU, 2U, buffer, 1024U, command),
                   M::Scsi::RangeCommand(true, 0U, 128U, buffer, 65536U, command), command.TransferLength == 65536U,
                   command.Bytes[8U] == 128U, !command.In);
    return context.Finish();
}
