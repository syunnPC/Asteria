// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_SCSI_HPP_INCLUDED
#define ASTERIA_USER_USB_SCSI_HPP_INCLUDED

#include "User/Usb/Bot.hpp"

namespace Asteria::User::Usb
{
enum class SenseStatus : UInt8
{
    Good,
    BecomingReady,
    MediumChanged,
    Reset,
    NoMedium,
    NotReady,
    IllegalRequest,
    WriteProtected,
    MediumError,
    Error,
};
struct Sense
{
    SenseStatus Status{SenseStatus::Error};
    UInt8 Key{0U};
    UInt8 Asc{0U};
    UInt8 Ascq{0U};
};
struct Inquiry
{
    UInt8 PeripheralType{0U};
    bool Removable{false};
    char Vendor[9U]{};
    char Product[17U]{};
};
struct Capacity
{
    UInt64 BlockCount{0U};
    UInt32 BlockSize{0U};
    bool Saturated{false};
};
/// Bounded failure-atomic byte decoders. Strings are ASCII, space-padded on
/// wire, NUL-terminated on output. Deferred sense is conservatively Error.
class Scsi final
{
public:
    [[nodiscard]] static bool DecodeSense(const UInt8* bytes, Size length, Sense& output) noexcept;
    [[nodiscard]] static bool DecodeInquiry(const UInt8* bytes, Size length, Inquiry& output) noexcept;
    [[nodiscard]] static bool Capacity10(const UInt8* bytes, Size length, Capacity& output) noexcept;
    [[nodiscard]] static bool Capacity16(const UInt8* bytes, Size length, Capacity& output) noexcept;
    [[nodiscard]] static bool ModeSense(const UInt8* bytes, Size length, bool ten, bool& readOnly) noexcept;
    [[nodiscard]] static ScsiCommand InquiryCommand() noexcept;
    [[nodiscard]] static ScsiCommand ReadyCommand() noexcept;
    [[nodiscard]] static ScsiCommand SenseCommand() noexcept;
    [[nodiscard]] static ScsiCommand CapacityCommand(bool sixteen) noexcept;
    [[nodiscard]] static ScsiCommand ModeCommand(bool ten) noexcept;
    [[nodiscard]] static ScsiCommand FlushCommand() noexcept;
    [[nodiscard]] static bool RangeCommand(bool write, UInt64 lba, UInt32 blocks, FrameBuffer buffer,
                                           UInt32 maximumBytes, ScsiCommand& output) noexcept;
};
}

#endif
