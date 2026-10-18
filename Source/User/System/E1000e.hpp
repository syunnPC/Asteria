// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_E1000E_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_E1000E_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::System
{
/// The Intel 82574L (e1000e) registers a driver of Source/User/Network/NetworkStack.hpp uses, as
/// 32-bit word indexes into BAR0 (Intel 82574 datasheet, document 317694, revision 3.4, section
/// 10.2). Every one lies below 0x8000, in the first eight pages. Accesses are whole 32-bit words:
/// ReadRegister and a writer of type E1000eRegisterWriter.
inline constexpr Foundation::Size E1000eCtrlWord{0x0000U >> 2U};
inline constexpr Foundation::Size E1000eStatusWord{0x0008U >> 2U};
inline constexpr Foundation::Size E1000eIcrWord{0x00C0U >> 2U};
inline constexpr Foundation::Size E1000eItrWord{0x00C4U >> 2U};
inline constexpr Foundation::Size E1000eImsWord{0x00D0U >> 2U};
inline constexpr Foundation::Size E1000eImcWord{0x00D8U >> 2U};
inline constexpr Foundation::Size E1000eRctlWord{0x0100U >> 2U};
inline constexpr Foundation::Size E1000eTctlWord{0x0400U >> 2U};
inline constexpr Foundation::Size E1000eRdbalWord{0x2800U >> 2U};
inline constexpr Foundation::Size E1000eRdbahWord{0x2804U >> 2U};
inline constexpr Foundation::Size E1000eRdlenWord{0x2808U >> 2U};
inline constexpr Foundation::Size E1000eRdhWord{0x2810U >> 2U};
inline constexpr Foundation::Size E1000eRdtWord{0x2818U >> 2U};
inline constexpr Foundation::Size E1000eRdtrWord{0x2820U >> 2U};
inline constexpr Foundation::Size E1000eRxdctlWord{0x2828U >> 2U};
inline constexpr Foundation::Size E1000eRadvWord{0x282CU >> 2U};
inline constexpr Foundation::Size E1000eTdbalWord{0x3800U >> 2U};
inline constexpr Foundation::Size E1000eTdbahWord{0x3804U >> 2U};
inline constexpr Foundation::Size E1000eTdlenWord{0x3808U >> 2U};
inline constexpr Foundation::Size E1000eTdhWord{0x3810U >> 2U};
inline constexpr Foundation::Size E1000eTdtWord{0x3818U >> 2U};
inline constexpr Foundation::Size E1000eTxdctlWord{0x3828U >> 2U};
inline constexpr Foundation::Size E1000eRfctlWord{0x5008U >> 2U};
inline constexpr Foundation::Size E1000eRal0Word{0x5400U >> 2U};
inline constexpr Foundation::Size E1000eRah0Word{0x5404U >> 2U};
/// The register words the first eight pages hold.
inline constexpr Foundation::Size E1000eMappedWords{0x8000U >> 2U};

/// CTRL: set link up, auto-speed detection, and the device reset, which
/// clears itself once done (82574 section 10.2.1.1, CTRL.RST).
inline constexpr Foundation::UInt32 E1000eCtrlSlu{1U << 6U};
inline constexpr Foundation::UInt32 E1000eCtrlAsde{1U << 5U};
inline constexpr Foundation::UInt32 E1000eCtrlRst{1U << 26U};
/// STATUS: the link is up.
inline constexpr Foundation::UInt32 E1000eStatusLu{1U << 1U};
/// ICR, IMS and IMC: a transmit descriptor written back, a link status
/// change, the receive ring below its threshold, a received frame (its
/// timer, zero here).
inline constexpr Foundation::UInt32 E1000eIntTxdw{1U << 0U};
inline constexpr Foundation::UInt32 E1000eIntLsc{1U << 2U};
inline constexpr Foundation::UInt32 E1000eIntRxdmt0{1U << 4U};
inline constexpr Foundation::UInt32 E1000eIntRxt0{1U << 7U};
/// RCTL: receive enabled, broadcasts accepted, the CRC stripped; buffer size
/// field zero (2 KiB) and no long packets.
inline constexpr Foundation::UInt32 E1000eRctlEn{1U << 1U};
inline constexpr Foundation::UInt32 E1000eRctlBam{1U << 15U};
inline constexpr Foundation::UInt32 E1000eRctlSecrc{1U << 26U};
/// TCTL: transmit enabled, short packets padded, the collision threshold (15)
/// and the collision distance. Intel 82574 section 10.2.6.1 defines CT+1
/// attempts and COLD+1 bytes: 15 permits 16 attempts and 63 pads to the
/// 64-byte minimum frame in full duplex and the 10/100 Mb/s collision window.
inline constexpr Foundation::UInt32 E1000eTctlEn{1U << 1U};
inline constexpr Foundation::UInt32 E1000eTctlPsp{1U << 3U};
inline constexpr Foundation::UInt32 E1000eTctlCt{0x0FU << 4U};
inline constexpr Foundation::UInt32 E1000eTctlCold{0x3FU << 12U};
/// TXDCTL bit 22 must be one for proper operation, despite its reserved
/// label (Intel 82574 section 10.2.6.11). RXDCTL bits 31:25 are reserved;
/// TXDCTL bits 31:25 are LWTHRESH, not a queue enable.
inline constexpr Foundation::UInt32 E1000eTxdctlRequired{1U << 22U};
/// RFCTL: extended receive descriptors; clear for legacy ones.
inline constexpr Foundation::UInt32 E1000eRfctlExten{1U << 15U};
/// RAH0: the low 16 bits are the address's last two bytes.
inline constexpr Foundation::UInt32 E1000eRahAddressMask{0xFFFFU};

/// Legacy descriptors (82574L sections 3.2.4 and 3.3.3), 16 bytes, encoded
/// field by field: the buffer's device address at 0, the length at 8;
/// receive: the status at 12 (DD, EOP) and the errors at 13; transmit: the
/// command at 11 (EOP, IFCS, RS) and the status at 12 (DD).
inline constexpr Foundation::Size E1000eDescriptorBytes{16U};
inline constexpr Foundation::Size E1000eDescriptorLengthOffset{8U};
inline constexpr Foundation::Size E1000eDescriptorCommandOffset{11U};
inline constexpr Foundation::Size E1000eDescriptorStatusOffset{12U};
inline constexpr Foundation::Size E1000eDescriptorErrorsOffset{13U};
inline constexpr Foundation::UInt8 E1000eStatusDd{1U << 0U};
inline constexpr Foundation::UInt8 E1000eStatusEop{1U << 1U};
inline constexpr Foundation::UInt8 E1000eCommandEop{1U << 0U};
inline constexpr Foundation::UInt8 E1000eCommandIfcs{1U << 1U};
inline constexpr Foundation::UInt8 E1000eCommandRs{1U << 3U};
/// Receive errors that spoil a frame: CRC, symbol, sequence and data errors
/// (the checksum errors, bits 5 and 6, are left to software).
inline constexpr Foundation::UInt8 E1000eFrameErrors{0x87U};

using E1000eRegisterReader = Foundation::UInt32 (*)(const volatile Foundation::UInt32*, Foundation::Size) noexcept;
using E1000eRegisterWriter = void (*)(volatile Foundation::UInt32*, Foundation::Size, Foundation::UInt32) noexcept;

/// Reads one whole register (ReadRegister, as a reader a hosted test can
/// replace: ICR clears as it is read).
[[nodiscard]] inline Foundation::UInt32 ReadE1000eRegister(const volatile Foundation::UInt32* const registers,
                                                           const Foundation::Size word) noexcept
{
    return ReadRegister(registers, word);
}

/// Writes one whole register. A supplied writer performs the same write,
/// with the device's synchronous side effects, before it returns.
inline void WriteE1000eRegister(volatile Foundation::UInt32* const registers, const Foundation::Size word,
                                const Foundation::UInt32 value) noexcept
{
    registers[word] = value;
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_E1000E_HPP_INCLUDED
