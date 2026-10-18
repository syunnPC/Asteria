// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_SYSTEM_FAKE_AHCI_HBA_HPP_INCLUDED
#define ASTERIA_TESTS_USER_SYSTEM_FAKE_AHCI_HBA_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::TestSupport
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;

struct RegisterWrite
{
    Size Word{0U};
    UInt32 Value{0U};
};

struct FakeAhciPortState
{
    UInt64 AssertedAt{0U};
    UInt64 PulseTicks{0U};
    UInt32 ResetCount{0U};
    UInt32 CrDelay{0U};
    UInt32 FrDelay{0U};
    UInt32 LinkDelay{0U};
    bool StuckCr{false};
    bool StuckFr{false};
    bool StuckLink{false};
    bool StuckCi{false};
    bool Device{false};
    bool SignaturePending{true};
};

// Adversarial controller model for Asteria's DMA lifetime contract. Stopping
// the command engine can clear CI while an issued operation still retains
// its descriptor addresses. The model schedules that completion separately;
// reset is its explicit cancellation boundary. These are test interleavings,
// not evidence about any particular emulator's implementation. All 32 ports
// have independent stop/reset fault injection. One selected DataPort owns
// the retained read's fixed DMA storage; other ports are empty by default.
// Store this large fake statically.
class FakeAhciHba final
{
    static constexpr UInt32 St{0x01U};
    static constexpr UInt32 Fre{0x10U};
    static constexpr UInt32 Fr{0x4000U};
    static constexpr UInt32 Cr{0x8000U};

public:
    UInt32 Words[0x1100U >> 2U]{};
    UInt8 Page[0x2500U]{};
    UInt8 Buffer[512U]{};
    RegisterWrite Writes[512U]{};
    FakeAhciPortState Ports[32U]{};
    Size WriteCount{0U};
    UInt64 Now{0U};
    UInt64 TickStep{1U};
    UInt64 Due{0U};
    UInt32 CompletionWrites{0U};
    UInt32 ResetCount{0U};
    UInt32 InFlightSlot{0U};
    UInt32 DataPort{2U};
    bool Pending{false};
    // QEMU recomputes HBA IS on PxIE/PxIS changes. Disable this only to
    // exercise a hardware-style latched HBA IS's write-one acknowledgement.
    bool RecomputeHbaStatus{true};

    [[nodiscard]] static constexpr Size PortWord(const UInt32 port) noexcept
    {
        return (0x100U >> 2U) + (static_cast<Size>(port) << 5U);
    }

    /// Resets fixed storage; ich9-ahci implements six ports, one with a disk.
    void Clear(const UInt32 dataPort = 2U) noexcept
    {
        for (UInt32& word : Words)
        {
            word = 0U;
        }
        for (UInt8& byte : Page)
        {
            byte = 0U;
        }
        for (UInt8& byte : Buffer)
        {
            byte = 0U;
        }
        for (FakeAhciPortState& state : Ports)
        {
            state = {};
        }
        WriteCount = 0U;
        Now = 0U;
        TickStep = 1U;
        Due = 0U;
        CompletionWrites = 0U;
        ResetCount = 0U;
        InFlightSlot = 0U;
        DataPort = dataPort;
        Pending = false;
        RecomputeHbaStatus = true;
        Ports[dataPort].Device = true;
        Words[3U] = 0x3FU;
        Words[PortWord(dataPort) + 10U] = 0x113U;
        for (UInt32 port{0U}; port < 32U; ++port)
        {
            Words[PortWord(port) + 9U] = 0xFFFFFFFFU;
        }
    }

    /// One deterministic clock tick; a stopped engine does not cancel DMA.
    [[nodiscard]] UInt64 Tick() noexcept
    {
        Now += TickStep; // Unsigned wrap is intentional, as in the caller's tick counter.
        for (UInt32 port{0U}; port < 32U; ++port)
        {
            FakeAhciPortState& state = Ports[port];
            const Size base = PortWord(port);
            const Size cmd = base + 6U;
            const Size ssts = base + 10U;
            if (((Words[cmd] & St) == 0U) && !state.StuckCr && (state.CrDelay != 0U))
            {
                --state.CrDelay;
                if (state.CrDelay == 0U)
                {
                    Words[cmd] &= ~Cr;
                }
            }
            if (((Words[cmd] & Fre) == 0U) && !state.StuckFr && (state.FrDelay != 0U))
            {
                --state.FrDelay;
                if (state.FrDelay == 0U)
                {
                    Words[cmd] &= ~Fr;
                }
            }
            if ((state.ResetCount != 0U) && !state.StuckLink && (state.LinkDelay != 0U))
            {
                --state.LinkDelay;
                if (state.LinkDelay == 0U)
                {
                    Words[ssts] = state.Device ? 0x113U : 0U;
                }
            }
        }
        if (Pending && (Now == Due))
        {
            Complete();
        }
        return Now;
    }

    /// Resolves retained DMA storage at issue, then schedules completion
    /// it independently of ST and the current command-list mapping.
    void Complete() noexcept
    {
        if (!Pending)
        {
            return;
        }
        Pending = false;
        const Size base = PortWord(DataPort);
        const Size cmd = base + 6U;
        const Size ci = base + 14U;
        const Size tfd = base + 8U;
        const Size is = base + 4U;
        for (UInt8& byte : Buffer)
        {
            byte = 0xA5U;
        }
        const Size header = static_cast<Size>(InFlightSlot) << 5U;
        Page[header + 4U] = 0U;
        Page[header + 5U] = 2U;
        Page[header + 6U] = 0U;
        Page[header + 7U] = 0U;
        CompletionWrites += 2U;
        Words[ci] &= ~(1U << InFlightSlot);
        Words[tfd] = 0x50U;
        if ((Words[cmd] & Fre) != 0U)
        {
            Page[0x440U] = 0x34U;
            Page[0x441U] = 0x40U;
            Page[0x442U] = 0x50U;
            Words[is] |= 1U;
            CheckInterrupts();
            ++CompletionWrites;
        }
    }

    /// Handles write-one status/issue bits and synchronous COMRESET drain.
    void Write(const Size word, const UInt32 value) noexcept
    {
        if (WriteCount < 512U)
        {
            Writes[WriteCount] = RegisterWrite{.Word = word, .Value = value};
        }
        ++WriteCount;
        if (word < (0x100U >> 2U))
        {
            if (word == 2U)
            {
                Words[word] &= ~value;
            }
            else
            {
                Words[word] = value;
            }
            CheckInterrupts();
            return;
        }
        const UInt32 port = static_cast<UInt32>((word - (0x100U >> 2U)) >> 5U);
        FakeAhciPortState& state = Ports[port];
        const Size base = PortWord(port);
        const Size cmd = base + 6U;
        const Size ci = base + 14U;
        const Size sctl = base + 11U;
        const Size ssts = base + 10U;
        const Size sig = base + 9U;
        const Size tfd = base + 8U;
        const Size is = base + 4U;
        const Size serr = base + 12U;
        if ((word == is) || (word == serr))
        {
            Words[word] &= ~value;
        }
        else if (word == cmd)
        {
            if (((Words[cmd] & St) != 0U) && ((value & St) == 0U))
            {
                Words[ci] = 0U;
            }
            Words[cmd] = (Words[cmd] & (Cr | Fr)) | (value & ~(Cr | Fr));
            if ((value & St) != 0U)
            {
                Words[cmd] |= Cr;
            }
            else if (!state.StuckCr && (state.CrDelay == 0U))
            {
                Words[cmd] &= ~Cr;
            }
            if ((value & Fre) != 0U)
            {
                Words[cmd] |= Fr;
                if (state.SignaturePending && state.Device)
                {
                    Words[sig] = 0x00000101U;
                    Words[tfd] = 0x130U;
                    Page[0x440U] = 0x34U;
                    Words[is] |= 1U;
                    CheckInterrupts();
                    state.SignaturePending = false;
                }
            }
            else if (!state.StuckFr && (state.FrDelay == 0U))
            {
                Words[cmd] &= ~Fr;
            }
        }
        else if (word == sctl)
        {
            if ((value & 0x0FU) == 1U)
            {
                state.AssertedAt = Now;
            }
            if (((Words[sctl] & 0x0FU) == 1U) && ((value & 0x0FU) == 0U))
            {
                // This completes before the write hook returns, not on Tick.
                if (port == DataPort)
                {
                    Complete();
                }
                ++ResetCount;
                ++state.ResetCount;
                state.PulseTicks = Now - state.AssertedAt;
                Words[ci] = state.StuckCi ? 1U : 0U;
                Words[serr] = 0U;
                Words[tfd] = 0x7FU;
                Words[sig] = 0xFFFFFFFFU;
                Words[ssts] = (state.StuckLink || (state.LinkDelay != 0U)) ? 1U : (state.Device ? 0x113U : 0U);
                state.SignaturePending = true;
                if (((Words[cmd] & Fre) != 0U) && state.Device)
                {
                    Words[sig] = 0x101U;
                    Words[tfd] = 0x130U;
                    Page[0x440U] = 0x34U;
                    Words[is] |= 1U;
                    CheckInterrupts();
                    state.SignaturePending = false;
                }
            }
            Words[sctl] = value;
        }
        else if (word == ci)
        {
            Words[ci] |= value;
            if (((Words[cmd] & St) != 0U) && !Pending && (port == DataPort))
            {
                for (UInt32 slot{0U}; slot < 32U; ++slot)
                {
                    if ((value & (1U << slot)) != 0U)
                    {
                        InFlightSlot = slot;
                        Pending = true;
                        Due = Now + 100U;
                        Words[tfd] = 0x80U;
                        break;
                    }
                }
            }
        }
        else
        {
            Words[word] = value;
        }
        CheckInterrupts();
    }

private:
    void CheckInterrupts() noexcept
    {
        if (!RecomputeHbaStatus)
        {
            return;
        }
        UInt32 pending{0U};
        for (UInt32 port{0U}; port < 32U; ++port)
        {
            const Size base = PortWord(port);
            if (((Words[3U] & (1U << port)) != 0U) && ((Words[base + 4U] & Words[base + 5U]) != 0U))
            {
                pending |= 1U << port;
            }
        }
        Words[2U] = pending;
    }
};

} // namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_USER_SYSTEM_FAKE_AHCI_HBA_HPP_INCLUDED
