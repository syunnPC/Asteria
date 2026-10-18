// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_SYSTEM_FAKE_E1000E_HPP_INCLUDED
#define ASTERIA_TESTS_USER_SYSTEM_FAKE_E1000E_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::TestSupport
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;

// Bounded host model of the Intel 82574, document 317694 revision 3.4:
// section 10.2 gives the register fields; 3.2.4 and 3.3.3 give the legacy
// descriptors; 7.4.3/7.4.5 give interrupt causes, masks and acknowledgement. The test clock
// makes reset and link completion deterministic. Each submitted buffer is
// logged or received through the test IOMMU (Translate). Literal offsets
// are independent of the production header. Store this large fake statically.
class FakeE1000e final
{
public:
    // The host address of bytes bytes at a device address, or nullptr when
    // the IOMMU (or bus mastering) refuses the access.
    using Translate = UInt8* (*)(UInt64 deviceAddress, Size bytes, bool write) noexcept;

    static constexpr Size Ctrl{0x0000U >> 2U};
    static constexpr Size Status{0x0008U >> 2U};
    static constexpr Size Icr{0x00C0U >> 2U};
    static constexpr Size Itr{0x00C4U >> 2U};
    static constexpr Size Ics{0x00C8U >> 2U};
    static constexpr Size Ims{0x00D0U >> 2U};
    static constexpr Size Imc{0x00D8U >> 2U};
    static constexpr Size Rctl{0x0100U >> 2U};
    static constexpr Size Tctl{0x0400U >> 2U};
    static constexpr Size Rdbal{0x2800U >> 2U};
    static constexpr Size Rdbah{0x2804U >> 2U};
    static constexpr Size Rdlen{0x2808U >> 2U};
    static constexpr Size Rdh{0x2810U >> 2U};
    static constexpr Size Rdt{0x2818U >> 2U};
    static constexpr Size Rdtr{0x2820U >> 2U};
    static constexpr Size Rxdctl{0x2828U >> 2U};
    static constexpr Size Radv{0x282CU >> 2U};
    static constexpr Size Tdbal{0x3800U >> 2U};
    static constexpr Size Tdbah{0x3804U >> 2U};
    static constexpr Size Tdlen{0x3808U >> 2U};
    static constexpr Size Tdh{0x3810U >> 2U};
    static constexpr Size Tdt{0x3818U >> 2U};
    static constexpr Size Txdctl{0x3828U >> 2U};
    static constexpr Size Rfctl{0x5008U >> 2U};
    static constexpr Size Ral0{0x5400U >> 2U};
    static constexpr Size Rah0{0x5404U >> 2U};
    static constexpr Size RegisterWords{0x8000U >> 2U};
    static constexpr UInt32 Rst{1U << 26U};
    static constexpr UInt32 Lu{1U << 1U};
    static constexpr UInt32 Txdw{1U << 0U};
    static constexpr UInt32 Rxt0{1U << 7U};
    static constexpr UInt32 RctlEn{1U << 1U};
    static constexpr UInt32 TctlEn{1U << 1U};
    static constexpr Size FrameCapacity{2048U};
    static constexpr Size Frames{24U};

    UInt32 Words[RegisterWords]{};
    // The wire: every frame transmitted, in order (the first Frames kept).
    UInt8 Wire[Frames][FrameCapacity]{};
    Size WireBytes[Frames]{};
    Size Transmitted{0U};
    // Frames from the wire not yet in the receive ring, oldest first.
    UInt8 Inbound[Frames][FrameCapacity]{};
    Size InboundBytes[Frames]{};
    // The status (DD and EOP) and errors each inbound frame's descriptor is
    // written back with.
    UInt8 InboundStatus[Frames]{};
    UInt8 InboundErrors[Frames]{};
    Size InboundHead{0U};
    Size InboundTail{0U};
    Size Received{0U};
    // Messages signalled, ICR reads, resets, DMA accesses refused.
    Size Messages{0U};
    Size IcrReads{0U};
    Size Resets{0U};
    Size DmaFaults{0U};
    Size InvalidDescriptorControls{0U};
    UInt64 Now{0U};
    UInt64 LinkDue{0U};
    UInt64 LinkTicks{3U};
    bool StuckReset{false};
    bool Absent{false};
    Translate Dma{nullptr};
    UInt8 Mac[6U]{};

    // Powers the device on with mac, its link LinkTicks after now.
    void Clear(const Translate dma, const UInt8 (&mac)[6U]) noexcept
    {
        for (UInt32& word : Words)
        {
            word = 0U;
        }
        for (Size index{0U}; index < Frames; ++index)
        {
            WireBytes[index] = 0U;
            InboundBytes[index] = 0U;
            InboundStatus[index] = 0U;
            InboundErrors[index] = 0U;
        }
        Transmitted = 0U;
        InboundHead = 0U;
        InboundTail = 0U;
        Received = 0U;
        Messages = 0U;
        IcrReads = 0U;
        Resets = 0U;
        DmaFaults = 0U;
        InvalidDescriptorControls = 0U;
        Now = 0U;
        LinkTicks = 3U;
        StuckReset = false;
        Absent = false;
        Dma = dma;
        for (Size index{0U}; index < 6U; ++index)
        {
            Mac[index] = mac[index];
        }
        Reset();
        Resets = 0U;
    }

    // One clock tick: the link comes up when due.
    [[nodiscard]] UInt64 Tick() noexcept
    {
        ++Now;
        if ((LinkDue != 0U) && (Now >= LinkDue))
        {
            LinkDue = 0U;
            Words[Status] |= Lu;
            Receive();
        }
        return Now;
    }

    [[nodiscard]] UInt32 Read(const Size word) noexcept
    {
        if (Absent)
        {
            return ~UInt32{0U};
        }
        if (word != Icr)
        {
            return Words[word];
        }
        ++IcrReads;
        const UInt32 value = Words[Icr];
        Words[Icr] = 0U;
        return value;
    }

    void Write(const Size word, const UInt32 value) noexcept
    {
        switch (word)
        {
        case Ctrl:
            Words[Ctrl] = value & ~Rst;
            if ((value & Rst) != 0U)
            {
                if (StuckReset)
                {
                    Words[Ctrl] |= Rst;
                }
                else
                {
                    Reset();
                }
            }
            return;
        case Status:
            return;
        case Ims:
            Raise(Ims, value);
            return;
        case Imc:
            Words[Ims] &= ~value;
            return;
        case Icr:
            Words[Icr] &= ~value;
            return;
        case Ics:
            Raise(Icr, value);
            return;
        case Rxdctl:
            // Section 10.2.5.11: reserved fields must be zero; WTHRESH
            // needs a nonzero delay timer when batching write-backs.
            if (((value & 0xFEC0'C0C0U) != 0U) ||
                (((value & 0x003F'0000U) != 0U) && (Words[Rdtr] == 0U) && (Words[Radv] == 0U)))
            {
                ++InvalidDescriptorControls;
            }
            Words[word] = value;
            return;
        case Txdctl:
            // Section 10.2.6.11: reserved bit 22 must be one.
            if (((value & (1U << 22U)) == 0U) || ((value & 0x0080'C0C0U) != 0U))
            {
                ++InvalidDescriptorControls;
            }
            Words[word] = value;
            return;
        case Rdt:
            Words[Rdt] = value & 0xFFFFU;
            Receive();
            return;
        case Tdt:
            Words[Tdt] = value & 0xFFFFU;
            Transmit();
            return;
        case Rctl:
            Words[Rctl] = value;
            Receive();
            return;
        case Tctl:
            Words[Tctl] = value;
            Transmit();
            return;
        default:
            Words[word] = value;
            return;
        }
    }

    // A frame arrives from the wire: into the receive ring if a descriptor
    // is free, else held until the driver gives one back. Its descriptor is
    // written back with status and errors (a whole, clean frame by default).
    void Inject(const UInt8* const frame, const Size bytes, const UInt8 status = 0x03U,
                const UInt8 errors = 0U) noexcept
    {
        if ((InboundTail - InboundHead) >= Frames)
        {
            return;
        }
        UInt8* const slot = Inbound[InboundTail % Frames];
        for (Size index{0U}; (index < bytes) && (index < FrameCapacity); ++index)
        {
            slot[index] = frame[index];
        }
        InboundBytes[InboundTail % Frames] = bytes;
        InboundStatus[InboundTail % Frames] = status;
        InboundErrors[InboundTail % Frames] = errors;
        ++InboundTail;
        Receive();
    }

private:
    static constexpr Size DescriptorBytes{16U};

    [[nodiscard]] static UInt64 Load(const UInt8* const bytes, const Size count) noexcept
    {
        UInt64 value{0U};
        for (Size index{0U}; index < count; ++index)
        {
            value |= static_cast<UInt64>(bytes[index]) << (index << 3U);
        }
        return value;
    }

    // Intel 82574 section 10.2 reset values: the receive address
    // restored, the link down until LinkTicks from now.
    void Reset() noexcept
    {
        for (UInt32& word : Words)
        {
            word = 0U;
        }
        Words[Ctrl] = 0x0000'0241U;
        Words[Itr] = 500U;
        Words[Rxdctl] = 1U << 16U;
        Words[0x3840U >> 2U] = 0x403U;
        Words[Ral0] = static_cast<UInt32>(Mac[0U]) | (static_cast<UInt32>(Mac[1U]) << 8U) |
                      (static_cast<UInt32>(Mac[2U]) << 16U) | (static_cast<UInt32>(Mac[3U]) << 24U);
        Words[Rah0] = static_cast<UInt32>(Mac[4U]) | (static_cast<UInt32>(Mac[5U]) << 8U) | 0x8000'0000U;
        LinkDue = Now + LinkTicks;
        ++Resets;
    }

    // e1000e_raise_interrupts: one message when a cause neither held nor
    // masked before becomes pending and unmasked.
    void Raise(const Size index, const UInt32 causes) noexcept
    {
        const UInt32 old = Words[Ims] & Words[Icr];
        Words[index] |= causes;
        if (((Words[Ims] & Words[Icr]) & ~old) != 0U)
        {
            ++Messages;
        }
    }

    [[nodiscard]] Size RingEntries(const Size length) const noexcept
    {
        return Words[length] / DescriptorBytes;
    }

    [[nodiscard]] UInt64 RingBase(const Size low, const Size high) const noexcept
    {
        return static_cast<UInt64>(Words[low]) | (static_cast<UInt64>(Words[high]) << 32U);
    }

    // Intel 82574 section 3.3.3: legacy transmit descriptors, one buffer each.
    void Transmit() noexcept
    {
        const Size entries = RingEntries(Tdlen);
        bool written{false};
        while (((Words[Tctl] & TctlEn) != 0U) && (entries != 0U) && (Words[Tdh] != Words[Tdt]) &&
               (Words[Tdt] < entries))
        {
            const UInt64 address = RingBase(Tdbal, Tdbah) + (static_cast<UInt64>(Words[Tdh]) * DescriptorBytes);
            UInt8* const descriptor = Dma(address, DescriptorBytes, false);
            if (descriptor == nullptr)
            {
                ++DmaFaults;
                return;
            }
            const Size length = static_cast<Size>(Load(descriptor + 8U, 2U));
            const UInt8* const buffer = Dma(Load(descriptor, 8U), length, false);
            if (buffer == nullptr)
            {
                ++DmaFaults;
            }
            else if (Transmitted < Frames)
            {
                for (Size index{0U}; (index < length) && (index < FrameCapacity); ++index)
                {
                    Wire[Transmitted][index] = buffer[index];
                }
                WireBytes[Transmitted] = length;
            }
            Transmitted += (buffer == nullptr) ? 0U : 1U;
            if ((descriptor[11U] & 0x08U) != 0U)
            {
                UInt8* const status = Dma(address + 12U, 1U, true);
                if (status != nullptr)
                {
                    *status |= 0x01U;
                    written = true;
                }
            }
            Words[Tdh] = static_cast<UInt32>((Words[Tdh] + 1U) % entries);
        }
        if (written)
        {
            Raise(Icr, Txdw);
        }
    }

    // e1000e_receive_internal: while a frame waits and the device owns a
    // descriptor (RDH short of RDT), the frame goes into its buffer and the
    // descriptor is written back with DD and EOP.
    void Receive() noexcept
    {
        const Size entries = RingEntries(Rdlen);
        bool written{false};
        while (((Words[Rctl] & RctlEn) != 0U) && ((Words[Status] & Lu) != 0U) && (entries != 0U) &&
               (Words[Rdt] < entries) && (Words[Rdh] != Words[Rdt]) && (InboundHead != InboundTail))
        {
            const UInt64 address = RingBase(Rdbal, Rdbah) + (static_cast<UInt64>(Words[Rdh]) * DescriptorBytes);
            UInt8* const descriptor = Dma(address, DescriptorBytes, true);
            if (descriptor == nullptr)
            {
                ++DmaFaults;
                return;
            }
            const Size bytes = InboundBytes[InboundHead % Frames];
            UInt8* const buffer = Dma(Load(descriptor, 8U), bytes, true);
            if (buffer == nullptr)
            {
                ++DmaFaults;
                return;
            }
            const UInt8* const frame = Inbound[InboundHead % Frames];
            for (Size index{0U}; index < bytes; ++index)
            {
                buffer[index] = frame[index];
            }
            descriptor[8U] = static_cast<UInt8>(bytes);
            descriptor[9U] = static_cast<UInt8>(bytes >> 8U);
            descriptor[10U] = 0U;
            descriptor[11U] = 0U;
            descriptor[12U] = InboundStatus[InboundHead % Frames];
            descriptor[13U] = InboundErrors[InboundHead % Frames];
            ++InboundHead;
            ++Received;
            written = true;
            Words[Rdh] = static_cast<UInt32>((Words[Rdh] + 1U) % entries);
        }
        if (written)
        {
            Raise(Icr, Rxt0);
        }
    }
};
} // namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_USER_SYSTEM_FAKE_E1000E_HPP_INCLUDED
