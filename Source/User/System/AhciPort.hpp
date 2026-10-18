// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SYSTEM_AHCI_PORT_HPP_INCLUDED
#define ASTERIA_USER_SYSTEM_AHCI_PORT_HPP_INCLUDED

#include "Abi/Clock.hpp"
#include "Foundation/Types.hpp"
#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::System
{
/// AHCI 1.3.1 sections 3.1 and 3.3: ABAR offsets in 32-bit words.
inline constexpr Foundation::Size AhciCapWord{0x00U >> 2U};
inline constexpr Foundation::Size AhciGhcWord{0x04U >> 2U};
inline constexpr Foundation::Size AhciIsWord{0x08U >> 2U};
inline constexpr Foundation::Size AhciPiWord{0x0CU >> 2U};
inline constexpr Foundation::Size AhciVsWord{0x10U >> 2U};
inline constexpr Foundation::Size AhciPortBaseWord{0x100U >> 2U};
inline constexpr Foundation::Size AhciClbWord{0x00U >> 2U};
inline constexpr Foundation::Size AhciClbuWord{0x04U >> 2U};
inline constexpr Foundation::Size AhciFbWord{0x08U >> 2U};
inline constexpr Foundation::Size AhciFbuWord{0x0CU >> 2U};
inline constexpr Foundation::Size AhciPortIsWord{0x10U >> 2U};
inline constexpr Foundation::Size AhciPortIeWord{0x14U >> 2U};
inline constexpr Foundation::Size AhciCmdWord{0x18U >> 2U};
inline constexpr Foundation::Size AhciTfdWord{0x20U >> 2U};
inline constexpr Foundation::Size AhciSigWord{0x24U >> 2U};
inline constexpr Foundation::Size AhciSstsWord{0x28U >> 2U};
inline constexpr Foundation::Size AhciSctlWord{0x2CU >> 2U};
inline constexpr Foundation::Size AhciSerrWord{0x30U >> 2U};
inline constexpr Foundation::Size AhciCiWord{0x38U >> 2U};
/// AHCI 1.3.1 section 3.1.1, CAP.S64A;
inline constexpr Foundation::UInt32 AhciS64a{1U << 31U};
inline constexpr Foundation::UInt32 AhciAe{1U << 31U};
inline constexpr Foundation::UInt32 AhciHr{1U};
inline constexpr Foundation::UInt32 AhciIe{1U << 1U};
inline constexpr Foundation::UInt32 AhciSt{1U};
inline constexpr Foundation::UInt32 AhciFre{1U << 4U};
inline constexpr Foundation::UInt32 AhciFr{1U << 14U};
inline constexpr Foundation::UInt32 AhciCr{1U << 15U};
inline constexpr Foundation::UInt32 AhciBsy{1U << 7U};
inline constexpr Foundation::UInt32 AhciDrq{1U << 3U};
inline constexpr Foundation::UInt32 AhciErr{1U};
inline constexpr Foundation::UInt32 AhciDetMask{0x0FU};
inline constexpr Foundation::UInt32 AhciDevicePresent{3U};
inline constexpr Foundation::UInt32 AhciTfes{1U << 30U};

/// Driver-owned DMA storage (AHCI 1.3.1 section 4.2). All 32 tables need
/// 0x2500 bytes, more than one 4 KiB page; slots 0..10 fit in that page.
inline constexpr Foundation::Size AhciReceivedFisOffset{0x400U};
inline constexpr Foundation::Size AhciCommandTablesOffset{0x500U};
inline constexpr Foundation::Size AhciPrdtOffset{0x80U};
inline constexpr Foundation::Size AhciDmaStorageBytes{0x2500U};

using AhciClock = Foundation::UInt64 (*)() noexcept;
using AhciRegisterWriter = void (*)(volatile Foundation::UInt32*, Foundation::Size, Foundation::UInt32) noexcept;

/// Writes one whole MMIO register. A supplied writer must perform the same
/// ordered write, including synchronous device side effects, before returning.
inline void WriteAhciRegister(volatile Foundation::UInt32* const registers, const Foundation::Size word,
                              const Foundation::UInt32 value) noexcept
{
    registers[word] = value;
}

/// The port's first ABAR word (AHCI 1.3.1 section 3.3); port must be below 32.
[[nodiscard]] inline constexpr Foundation::Size AhciPortWord(const Foundation::UInt32 port) noexcept
{
    return AhciPortBaseWord + (static_cast<Foundation::Size>(port) << 5U);
}

/// Waits for masked bits to clear within the shared tick budget. The clock
/// must advance during polling; unsigned subtraction allows a counter wrap.
[[nodiscard]] inline bool WaitAhciRegisterClear(const volatile Foundation::UInt32* const registers,
                                                const Foundation::Size word, const Foundation::UInt32 mask,
                                                const AhciClock clock, const Foundation::UInt64 start,
                                                const Foundation::UInt64 budget) noexcept
{
    while ((ReadRegister(registers, word) & mask) != 0U)
    {
        if ((Abi::ElapsedClockTicks(clock(), start)) >= budget)
        {
            return false;
        }
    }
    return true;
}

/// Stops ST/CR before FRE/FR (AHCI 1.3.1 sections 10.3.1 and 10.3.2), using one tick budget for
/// both waits. Success is not DMA quiescence on QEMU: AHCI block I/O require COMRESET before live
/// storage is released.
[[nodiscard]] inline bool StopAhciPort(volatile Foundation::UInt32* const registers, const Foundation::UInt32 port,
                                       const AhciClock clock, const Foundation::UInt64 budget,
                                       const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    if (port >= 32U)
    {
        return false;
    }
    const Foundation::Size commandWord = AhciPortWord(port) + AhciCmdWord;
    const Foundation::UInt64 start = clock();
    writeRegister(registers, commandWord, ReadRegister(registers, commandWord) & ~AhciSt);
    if (!WaitAhciRegisterClear(registers, commandWord, AhciCr, clock, start, budget))
    {
        return false;
    }
    writeRegister(registers, commandWord, ReadRegister(registers, commandWord) & ~AhciFre);
    return WaitAhciRegisterClear(registers, commandWord, AhciFr, clock, start, budget);
}

/// Starts a stopped port with caller-owned, device-mapped CLB (1 KiB aligned)
/// and FIS (256-byte aligned) storage; false leaves registers untouched.
/// The caller ensures a ready device and supported address width. AHCI 1.3.1
/// sections 10.1 and 10.3: AE first, valid bases, then FRE before ST.
[[nodiscard]] inline bool StartAhciPort(volatile Foundation::UInt32* const registers, const Foundation::UInt32 port,
                                        const Foundation::UInt64 commandListDevice, const Foundation::UInt64 fisDevice,
                                        const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    if ((port >= 32U) || ((commandListDevice & 0x3FFU) != 0U) || ((fisDevice & 0xFFU) != 0U))
    {
        return false;
    }
    const Foundation::Size base = AhciPortWord(port);
    if ((ReadRegister(registers, base + AhciCmdWord) & (AhciSt | AhciCr | AhciFre | AhciFr)) != 0U)
    {
        return false;
    }
    const Foundation::UInt32 control = ReadRegister(registers, AhciGhcWord);
    if ((control & AhciAe) == 0U)
    {
        writeRegister(registers, AhciGhcWord, control | AhciAe);
    }
    writeRegister(registers, base + AhciClbWord, static_cast<Foundation::UInt32>(commandListDevice & 0xFFFFFFFFU));
    writeRegister(registers, base + AhciClbuWord, static_cast<Foundation::UInt32>(commandListDevice >> 32U));
    writeRegister(registers, base + AhciFbWord, static_cast<Foundation::UInt32>(fisDevice & 0xFFFFFFFFU));
    writeRegister(registers, base + AhciFbuWord, static_cast<Foundation::UInt32>(fisDevice >> 32U));
    writeRegister(registers, base + AhciSerrWord, 0xFFFFFFFFU);
    writeRegister(registers, base + AhciPortIsWord, 0xFFFFFFFFU);
    writeRegister(registers, base + AhciCmdWord, ReadRegister(registers, base + AhciCmdWord) | AhciFre);
    writeRegister(registers, base + AhciCmdWord, ReadRegister(registers, base + AhciCmdWord) | AhciSt);
    return true;
}

/// Recovery quiesce, never request cancellation (AHCI block I/O; AHCI 1.3.1 section 10.4.2).
/// Attempts a stop for budget ticks, then allows a fresh budget for COMRESET and link recovery even
/// if CR was stuck. resetTicks must represent at least 1 ms in the caller's advancing clock. DET is
/// released even on timeout; false never authorizes releasing DMA. On QEMU DET 1 -> 0 drains
/// pending DMA before the write returns. The caller masks interrupts and acknowledges HBA IS after
/// all implemented ports reset.
[[nodiscard]] inline bool ResetAhciPort(volatile Foundation::UInt32* const registers, const Foundation::UInt32 port,
                                        const AhciClock clock, const Foundation::UInt64 budget,
                                        const Foundation::UInt64 resetTicks,
                                        const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    if ((port >= 32U) || (resetTicks == 0U) || (resetTicks > budget))
    {
        return false;
    }
    static_cast<void>(StopAhciPort(registers, port, clock, budget, writeRegister));
    const Foundation::Size base = AhciPortWord(port);
    const Foundation::UInt32 control = ReadRegister(registers, base + AhciSctlWord) & ~AhciDetMask;
    writeRegister(registers, base + AhciSctlWord, control | 1U);
    const Foundation::UInt64 start = clock();
    Foundation::UInt64 elapsed{0U};
    do
    {
        elapsed = Abi::ElapsedClockTicks(clock(), start);
    } while (elapsed < resetTicks);
    writeRegister(registers, base + AhciSctlWord, control);
    if (elapsed > budget)
    {
        return false;
    }
    for (;;)
    {
        const Foundation::UInt32 detected = ReadRegister(registers, base + AhciSstsWord) & AhciDetMask;
        if ((detected == AhciDevicePresent) || (detected == 0U))
        {
            break;
        }
        if ((Abi::ElapsedClockTicks(clock(), start)) >= budget)
        {
            return false;
        }
    }
    writeRegister(registers, base + AhciSerrWord, 0xFFFFFFFFU);
    writeRegister(registers, base + AhciPortIsWord, 0xFFFFFFFFU);
    return ReadRegister(registers, base + AhciCiWord) == 0U;
}

/// Encodes a DMA dword little-endian, without imposing alignment on storage.
inline void StoreAhciDword(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>((value >> (index << 3U)) & 0xFFU);
    }
}

namespace Detail
{
/// Clears a slot's command header and 256-byte table, then writes the header:
/// a five-dword command FIS, one PRDT entry, the write bit, and the table's
/// address. Returns the table. slot must be below 32.
inline Foundation::UInt8* PrepareAhciSlot(Foundation::UInt8* const page, const Foundation::UInt32 slot,
                                          const bool write, const Foundation::UInt64 tableDevice) noexcept
{
    Foundation::UInt8* const header = page + (static_cast<Foundation::Size>(slot) << 5U);
    Foundation::UInt8* const table = page + AhciCommandTablesOffset + (static_cast<Foundation::Size>(slot) << 8U);
    for (Foundation::Size index{0U}; index < 32U; ++index)
    {
        header[index] = 0U;
    }
    for (Foundation::Size index{0U}; index < 0x100U; ++index)
    {
        table[index] = 0U;
    }
    StoreAhciDword(header, 5U | (write ? 1U << 6U : 0U) | (1U << 16U));
    StoreAhciDword(header + 8U, static_cast<Foundation::UInt32>(tableDevice & 0xFFFFFFFFU));
    StoreAhciDword(header + 12U, static_cast<Foundation::UInt32>(tableDevice >> 32U));
    table[0U] = 0x27U;
    table[1U] = 0x80U;
    return table;
}
} // Namespace Detail

/// Builds one idle slot's IDENTIFY DEVICE (ATA command 0xEC; AHCI 1.3.1
/// sections 4.2.2 and 4.2.3), as the kernel's boot diagnostic encodes it
/// (Kernel/Device/AhciIdentifyDmaPageBuilder.hpp): no device bits, no count,
/// and one PRDT entry of the 512 bytes of IDENTIFY data at bufferDevice
/// (word aligned), without an interrupt on its completion. page and
/// tableDevice are as for BuildAhciReadWrite. Only the slot's header and
/// table are written; the data buffer is not cleared. Rejects invalid input
/// without writing; the caller makes these stores visible to the HBA before
/// issuing the slot.
[[nodiscard]] inline bool BuildAhciIdentify(Foundation::UInt8* const page, const Foundation::UInt32 slot,
                                            const Foundation::UInt64 bufferDevice,
                                            const Foundation::UInt64 tableDevice) noexcept
{
    if ((slot >= 32U) || ((bufferDevice & 1U) != 0U) || ((tableDevice & 0x7FU) != 0U))
    {
        return false;
    }
    Foundation::UInt8* const table = Detail::PrepareAhciSlot(page, slot, false, tableDevice);
    table[2U] = 0xECU;
    StoreAhciDword(table + AhciPrdtOffset, static_cast<Foundation::UInt32>(bufferDevice & 0xFFFFFFFFU));
    StoreAhciDword(table + AhciPrdtOffset + 4U, static_cast<Foundation::UInt32>(bufferDevice >> 32U));
    StoreAhciDword(table + AhciPrdtOffset + 12U, 511U);
    return true;
}

/// Builds one idle slot's READ/WRITE DMA EXT (AHCI 1.3.1 sections 4.2.2, 4.2.3.1 and 4.2.3.3; AHCI
/// block I/O ). page is borrowed DMA storage extending through 0x500 + (slot + 1) * 0x100;
/// tableDevice is that slot's table address, 128-byte aligned. Rejects invalid input without
/// writing. The caller supplies a contiguous buffer, keeps both mappings live, and makes these
/// stores visible to the HBA before issuing the slot.
[[nodiscard]] inline bool BuildAhciReadWrite(Foundation::UInt8* const page, const Foundation::UInt32 slot,
                                             const bool write, const Foundation::UInt64 lba,
                                             const Foundation::UInt32 sectors, const Foundation::UInt64 bufferDevice,
                                             const Foundation::UInt32 bytes,
                                             const Foundation::UInt64 tableDevice) noexcept
{
    if ((slot >= 32U) || (sectors == 0U) || (sectors > 128U) || (bytes != (sectors << 9U)) ||
        ((bufferDevice & 1U) != 0U) || ((lba >> 48U) != 0U) || ((tableDevice & 0x7FU) != 0U))
    {
        return false;
    }
    Foundation::UInt8* const table = Detail::PrepareAhciSlot(page, slot, write, tableDevice);
    table[2U] = write ? 0x35U : 0x25U;
    table[7U] = 0x40U;
    for (Foundation::UInt32 index{0U}; index < 3U; ++index)
    {
        table[4U + index] = static_cast<Foundation::UInt8>((lba >> (index << 3U)) & 0xFFU);
        table[8U + index] = static_cast<Foundation::UInt8>((lba >> ((index + 3U) << 3U)) & 0xFFU);
    }
    table[12U] = static_cast<Foundation::UInt8>(sectors & 0xFFU);
    table[13U] = static_cast<Foundation::UInt8>(sectors >> 8U);
    StoreAhciDword(table + AhciPrdtOffset, static_cast<Foundation::UInt32>(bufferDevice & 0xFFFFFFFFU));
    StoreAhciDword(table + AhciPrdtOffset + 4U, static_cast<Foundation::UInt32>(bufferDevice >> 32U));
    StoreAhciDword(table + AhciPrdtOffset + 12U, (bytes - 1U) | (1U << 31U));
    return true;
}

/// PowerOff.md / filesystem protocol: ATA8-ACS rev3f §7.15 FLUSH CACHE EXT (EAh), AHCI 1.3.1
/// §§4.2.2–4.2.3: non-data H2D FIS, PRDTL=0, CFL=5.
[[nodiscard]] inline bool BuildAhciFlush(Foundation::UInt8* const page, const Foundation::UInt32 slot,
                                         const Foundation::UInt64 tableDevice) noexcept
{
    if (slot >= 32U || (tableDevice & 0x7FU) != 0U)
    {
        return false;
    }
    auto* const table = Detail::PrepareAhciSlot(page, slot, false, tableDevice);
    StoreAhciDword(page + (static_cast<Foundation::Size>(slot) << 5U), 5U);
    table[2U] = 0xEAU;
    return true;
}
/// Issues one prepared slot; port and slot must be below 32 and the port
/// running. PxCI is write-one-to-set, not a read/modify/write (section 3.3).
inline void IssueAhciCommand(volatile Foundation::UInt32* const registers, const Foundation::UInt32 port,
                             const Foundation::UInt32 slot,
                             const AhciRegisterWriter writeRegister = WriteAhciRegister) noexcept
{
    writeRegister(registers, AhciPortWord(port) + AhciCiWord, 1U << slot);
}

/// The non-NCQ slot's issue bit cleared (section 3.3); valid port/slot required.
/// Only normal running completion implies DMA ended; stopping also clears
/// PxCI on QEMU. An error can leave PxCI set: check AhciCommandFailed too.
[[nodiscard]] inline bool IsAhciCommandDone(const volatile Foundation::UInt32* const registers,
                                            const Foundation::UInt32 port, const Foundation::UInt32 slot) noexcept
{
    return (ReadRegister(registers, AhciPortWord(port) + AhciCiWord) & (1U << slot)) == 0U;
}

/// Task-file failure from PxIS.TFES or PxTFD.ERR (AHCI 1.3.1 section 3.3).
/// port must be valid; a false result alone does not imply completion.
[[nodiscard]] inline bool AhciCommandFailed(const volatile Foundation::UInt32* const registers,
                                            const Foundation::UInt32 port) noexcept
{
    const Foundation::Size base = AhciPortWord(port);
    return ((ReadRegister(registers, base + AhciPortIsWord) & AhciTfes) != 0U) ||
           ((ReadRegister(registers, base + AhciTfdWord) & AhciErr) != 0U);
}
} // Namespace Asteria::User::System

#endif // ASTERIA_USER_SYSTEM_AHCI_PORT_HPP_INCLUDED
